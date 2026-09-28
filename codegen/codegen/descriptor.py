"""
Schema descriptor, fingerprint, and schema-history snapshots
(NATIVE_FILE_FORMAT_RESEARCH.md §3 "Schema descriptor" and §4.1).

The descriptor is the data-shape-only view of a Schema: classes, fields,
their storage kind and type, list/optional flags, parent relations,
index constraints, defaults and enum values. Everything that doesn't
change what a stored object *is* - descriptions, examples, TCL-surface
flags - is left out, so editing a docstring never looks like a schema
change.

It serves three purposes:
- embedded in the generated schema_version.hpp, so a native-format
  writer can copy it into every file it writes (a file carries the schema
  it was written with);
- hashed into the schema fingerprint, which identifies a schema shape
  independent of its version string;
- written to <history dir>/<version>.json, one snapshot per schema
  version - the "before" side of each future migration, and what the
  history check below compares against.
"""

import hashlib
import json
import re
from pathlib import Path
from typing import Any, Dict, List, Optional

from codegen.schema import TYPEMAP, Field, Klass, Schema

DESCRIPTOR_FORMAT = 1
FINGERPRINT_LENGTH = 16


def _field_kind(field: Field) -> str:
    """
    How a field is stored in its XxxData struct:
    - "parent": an Id of the owning object (the only stored side of a
      parent/child relationship)
    - "child":  the derived, index-maintained other side of a parent
      relationship - not stored in the struct at all
    - "ref":    a plain Id reference to another pooled object
    - "enum":   an enum value
    - "struct": an embedded (non-pooled) struct such as Rect or Point
    - "scalar": a TYPEMAP primitive (int, dbu, str, bool, ...)
    """
    if field.has_parent():
        return "parent"
    type_klass = field._type_klass
    if type_klass is not None:
        if field.is_child:
            return "child"
        if type_klass.is_enum:
            return "enum"
        if type_klass.has_pool:
            return "ref"
        return "struct"
    if field.type in TYPEMAP:
        return "scalar"
    raise ValueError(f"Field {field._klass.name if field._klass else '?'}.{field.name}: unknown type {field.type!r}")


def _stored_as_optional(field: Field) -> bool:
    if _field_kind(field) == "child":
        return False
    return field.get_cpp_type().startswith("std::optional<")


def _field_descriptor(field: Field) -> Dict[str, Any]:
    desc: Dict[str, Any] = {
        "name": field.name,
        "kind": _field_kind(field),
        "type": field.type,
    }
    if field.is_list:
        desc["list"] = True
    if field.is_optional:
        desc["optional"] = True
    # How the value is actually stored, stated explicitly rather than left
    # to a reader re-deriving it: a presence byte precedes the value only
    # when the C++ type is std::optional (a list is a plain vector, and a
    # reference an Id whose invalid value means "unset", whatever
    # is_optional says - see Field.get_cpp_type()).
    if _stored_as_optional(field):
        desc["presence"] = True
    if field.has_parent():
        desc["parent_field"] = field.parent
    if field.index:
        desc["index"] = True
    if field.unique_per_parent:
        desc["unique_per_parent"] = True
    if field.default is not None:
        desc["default"] = field.default
    return desc


def _klass_descriptor(klass: Klass) -> Dict[str, Any]:
    if klass.is_enum:
        return {
            "name": klass.name,
            "kind": "enum",
            # Enum values are declared as Fields carrying `value=` (what
            # enum_hpp_j2 renders), not Klass.enum_values.
            "values": [{"name": f.name, "value": f.value} for f in klass.fields],
        }
    return {
        "name": klass.name,
        "kind": "pooled" if klass.has_pool else "struct",
        "fields": [_field_descriptor(f) for f in klass.fields],
    }


def build_descriptor(schema: Schema) -> Dict[str, Any]:
    """
    The descriptor for `schema`, in declaration order (readable in a
    snapshot diff). The schema must already be linked (Schema.link()).
    """
    return {
        "format": DESCRIPTOR_FORMAT,
        "name": schema.name,
        "namespace": schema.namespace,
        "version": schema.version,
        "classes": [_klass_descriptor(k) for k in schema.classes],
    }


def _canonical_shape(descriptor: Dict[str, Any]) -> Dict[str, Any]:
    """
    The part of a descriptor the fingerprint covers: classes, fields and
    enum values sorted by name, version string excluded. Reordering
    declarations therefore doesn't change the fingerprint (the native
    format matches everything by name), and the fingerprint identifies a
    shape rather than a label.
    """
    classes = []
    for klass in sorted(descriptor["classes"], key=lambda k: k["name"]):
        klass = dict(klass)
        if "fields" in klass:
            klass["fields"] = sorted(klass["fields"], key=lambda f: f["name"])
        if "values" in klass:
            klass["values"] = sorted(klass["values"], key=lambda v: v["name"])
        classes.append(klass)
    return {"format": descriptor["format"], "namespace": descriptor["namespace"], "classes": classes}


def fingerprint(descriptor: Dict[str, Any]) -> str:
    """A short, stable hash of the descriptor's canonical shape."""
    canonical = json.dumps(_canonical_shape(descriptor), sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()[:FINGERPRINT_LENGTH]


def descriptor_json(descriptor: Dict[str, Any]) -> str:
    """Compact JSON form, as embedded in schema_version.hpp."""
    return json.dumps(descriptor, separators=(",", ":"))


# --- Schema history --------------------------------------------------------

_VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


def parse_version(version: str) -> tuple[int, int, int]:
    match = _VERSION_RE.match(version)
    if not match:
        raise ValueError(f"Schema version {version!r} is not MAJOR.MINOR.PATCH")
    return (int(match.group(1)), int(match.group(2)), int(match.group(3)))


def snapshot_path(history_dir: Path, version: str) -> Path:
    return history_dir / f"{version}.json"


def read_snapshots(history_dir: Path) -> Dict[str, Dict[str, Any]]:
    """Every snapshot in `history_dir`, keyed by version string."""
    snapshots: Dict[str, Dict[str, Any]] = {}
    if not history_dir.is_dir():
        return snapshots
    for path in history_dir.glob("*.json"):
        with open(path) as f:
            snapshot = json.load(f)
        if snapshot.get("version") != path.stem:
            raise ValueError(f"{path}: records version {snapshot.get('version')!r}, expected {path.stem!r}")
        snapshots[path.stem] = snapshot
    return snapshots


def write_snapshot(history_dir: Path, descriptor: Dict[str, Any]) -> Path:
    history_dir.mkdir(parents=True, exist_ok=True)
    path = snapshot_path(history_dir, descriptor["version"])
    snapshot = {
        "version": descriptor["version"],
        "fingerprint": fingerprint(descriptor),
        "descriptor": descriptor,
    }
    with open(path, "w") as f:
        json.dump(snapshot, f, indent=2)
        f.write("\n")
    return path


class HistoryCheck:
    """Outcome of check_history(): errors, and the snapshot to write (if any)."""

    def __init__(self) -> None:
        self.errors: List[str] = []
        self.write: bool = False
        self.note: Optional[str] = None


def check_history(descriptor: Dict[str, Any], history_dir: Path, update_snapshot: bool = False) -> HistoryCheck:
    """
    Compare `descriptor` against the snapshots in `history_dir`:

    - no snapshots yet: record this version as the baseline;
    - a snapshot for this version exists with the same fingerprint: fine;
    - a snapshot for this version exists with a different fingerprint: the
      schema changed without a version bump - an error, unless
      `update_snapshot` (for a version that hasn't been released or
      committed yet, while its schema is still being iterated on);
    - no snapshot for this version: it must be newer than every recorded
      version, and gets a new snapshot.
    """
    result = HistoryCheck()
    version = descriptor["version"]
    try:
        current = parse_version(version)
    except ValueError as e:
        result.errors.append(str(e))
        return result

    snapshots = read_snapshots(history_dir)
    current_fp = fingerprint(descriptor)

    if not snapshots:
        result.write = True
        result.note = f"Recorded baseline schema snapshot {version} (fingerprint {current_fp})"
        return result

    existing = snapshots.get(version)
    if existing is not None:
        if existing["fingerprint"] == current_fp:
            return result
        if update_snapshot:
            result.write = True
            result.note = f"Updated schema snapshot {version} (fingerprint {existing['fingerprint']} -> {current_fp})"
            return result
        result.errors.append(
            f"The schema changed (fingerprint {existing['fingerprint']} -> {current_fp}) but its version is still "
            f"{version}. Bump version= in the Schema(...) call. If {version} has not been committed or released "
            f"yet, rerun with --update-snapshot instead."
        )
        return result

    newest = max(snapshots, key=parse_version)
    if parse_version(newest) >= current:
        result.errors.append(
            f"Schema version {version} is not newer than the latest recorded snapshot {newest}. "
            f"Versions must only increase."
        )
        return result
    result.write = True
    result.note = f"Recorded new schema snapshot {version} (fingerprint {current_fp}, previous {newest})"
    if snapshots[newest]["fingerprint"] == current_fp:
        # Legitimate only for a data-meaning change the shape doesn't show
        # (e.g. a future migration that rescales a field's units).
        result.note += " - note: the schema shape is unchanged from the previous version"
    return result

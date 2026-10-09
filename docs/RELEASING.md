# Releasing Layout Engine

A release is a signed git tag `vX.Y.Z` on `main`. Pushing it runs
`.github/workflows/release.yml`, which builds the portable Linux bundle
(`Dockerfile.linux-release`), signs it, and publishes both to a GitHub
release.

One SSH key, the **release key**, signs both:
- the **tag** (git's SSH signing), so the source is attributable and GitHub
  shows it as Verified;
- the **bundle** (`ssh-keygen -Y sign`, done in CI from a repository
  secret), so a download can be checked before it is run.

The public half is committed as `assets/keys/allowed_signers`. CI checks every
bundle signature against it, and `le` checks a release bundle against the
keys a project trusts (see `tools/le/README.md`).

## One-time setup: the release key

Do these once, in order, before the first release.

### 1. Create the key

```
mkdir -p ~/.ssh/layout_engine && chmod 700 ~/.ssh/layout_engine
ssh-keygen -t ed25519 -N "" -C "Layout Engine release key" -f ~/.ssh/layout_engine/release_key
```

This writes `release_key` (private) and `release_key.pub` (public).

The key has no passphrase because CI has to use it unattended, and
`ssh-keygen -Y sign` can't answer a prompt. Protect it instead:
- Keep the private key only in `~/.ssh/layout_engine/` (mode 600, as
  `ssh-keygen` creates it), in the GitHub secret below, and in one offline
  backup, e.g. a password manager's secure-file storage.
- Never commit it, and never paste it into an issue, a PR or a chat.

### 2. Give CI the private key

```
gh secret set LE_RELEASE_SIGNING_KEY --repo drummondj/layout_engine < ~/.ssh/layout_engine/release_key
```

`gh secret list --repo drummondj/layout_engine` should now show
`LE_RELEASE_SIGNING_KEY`. Secrets can be written but never read back, so the
backup from step 1 is the only other copy.

### 3. Commit the public key

```
echo "release@layout-engine namespaces=\"git,layout_engine-release\" $(cut -d' ' -f1,2 ~/.ssh/layout_engine/release_key.pub)" > assets/keys/allowed_signers
```

Commit `assets/keys/allowed_signers` through a normal PR. The principal
`release@layout-engine` and the namespaces must stay exactly as written:
`git` covers tag signatures, and `layout_engine-release` covers bundle
signatures. CI's verify step uses both names.

### 4. Let git sign tags with it (this clone only)

```
git config gpg.format ssh
git config user.signingkey ~/.ssh/layout_engine/release_key.pub
git config gpg.ssh.allowedSignersFile assets/keys/allowed_signers
```

### 5. Add the public key to GitHub as a signing key

GitHub → Settings → SSH and GPG keys → New SSH key. Set **Key type** to
**Signing Key**, then paste the contents of `~/.ssh/layout_engine/release_key.pub`.
GitHub then marks tags signed with it as Verified. The tagger email (your
`git config user.email`) must be a verified email on your account.

## Making a release

1. **Bump the version.** In `CMakeLists.txt`, change
   `project(layout_engine_backend VERSION X.Y.Z ...)`. Merge it through a PR
   like any other change. The release workflow refuses a tag that doesn't
   match this version.
2. **Tag `main` and push the tag:**

   ```
   git switch main && git pull
   git tag -s vX.Y.Z -m "Layout Engine X.Y.Z"
   git verify-tag vX.Y.Z
   git push origin vX.Y.Z
   ```

3. **Watch the workflow** (`gh run watch`, or the Actions tab). It checks the
   tag against the project version, builds and smoke-tests the bundle, signs
   it, verifies the signature against `assets/keys/allowed_signers`, and only
   then publishes `layout_engine-linux-x86_64-vX.Y.Z.tar.gz` and its `.sig`.

To rehearse without publishing anything, run the workflow from the Actions
tab (**Run workflow** on any branch). It builds and signs the same way, but
uploads a workflow artifact instead of creating a release.

Never move or re-push a published tag. If a release is broken, fix it and
release a new version.

## Verifying a download

```
ssh-keygen -Y verify -f assets/keys/allowed_signers -I release@layout-engine \
    -n layout_engine-release -s layout_engine-linux-x86_64-vX.Y.Z.tar.gz.sig \
    < layout_engine-linux-x86_64-vX.Y.Z.tar.gz
```

It prints `Good "layout_engine-release" signature for release@layout-engine`
for an intact download. `git verify-tag vX.Y.Z` checks a tag the same way.

## Rotating or revoking the key

- **Rotation:** create a new key (step 1), then update the secret (step 2) and
  your git config (step 4). Then add a new line to `assets/keys/allowed_signers`.
  Mark the old line `valid-before="YYYYMMDD"` rather than deleting it, so
  releases it signed still verify. Swap the key on GitHub (step 5).
- **If the private key leaks:** delete the `LE_RELEASE_SIGNING_KEY` secret
  and the key on GitHub at once. Remove its line from `allowed_signers`
  (anything it signed can no longer be trusted), open an issue, create a new
  key, and re-release.

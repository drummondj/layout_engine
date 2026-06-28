Reading ffi data
================

  db.BrowserPayload? _browserInfo;
    await BackendPlugin.serializeLibraryBrowserData(id);
    final size = await BackendPlugin.getBrowserDataSize();
    final pointer = await BackendPlugin.getBrowserDataPtr();
    final Uint8List rawBytes = pointer.asTypedList(size);
    final browserInfo = db.BrowserPayload(rawBytes);
    debugPrint(browserInfo.toString());

  List<TableRow>? _buildTableRows() {
    if (_browserInfo == null || _browserInfo!.libraries == null) {
      return null;
    }

    List<TableRow> rows = [];

    for (final libraryInfo in _browserInfo!.libraries!) {
      if (libraryInfo.designs == null) {
        continue;
      }
      for (final designInfo in libraryInfo.designs!) {
        if (designInfo.views == null) {
          continue;
        }
        for (final viewInfo in designInfo.views!) {
          rows.add(
            TableRow(
              children: [
                Text(libraryInfo.name ?? "unknown"),
                Text(designInfo.name ?? "unknown"),
                Text(viewInfo.name ?? "unknown"),
              ],
            ),
          );
        }
      }
    }

    return rows;
  }


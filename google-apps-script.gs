function doGet(e) {

  var timestamp = new Date();
  updateCell("Last Sync", timestamp)
    .setNumberFormat("dd.mm.yyyy hh:mm");

  var secs = getParam(e, "uptime");
  var days = Math.floor(secs/86400);
  secs = secs%86400;
  var hours = Math.floor(secs/3600);
  secs = secs%3600;
  var mins = Math.floor(secs/60);
  secs = secs%60;
  updateCell("Uptime", Utilities.formatString(
    "%d days, %02d:%02d:%02d", days, hours, mins, secs))
    .setHorizontalAlignment("right");

  updateCell("Error Count", getParam(e, "errorCount"));

  var response = {}
  response["syncInterval"] = readCell("Sync Interval", 60);

  // Prepare to log temperatures
  var headerValues = ["Time"];
  var row = [timestamp];

  // Sync zone settings and temperatures
  getParams(e, "zone").forEach(it => {
    let zone = it.split(";");
    if (zone.length >= 4) {
      let zoneName      = zone[0];
      let zoneValue     = zone[1];
      let zoneTemp      = zone[2];
      let zoneDutyCycle = zone[3];

      // Reverse sync when a specific value is reported, device -> sheet
      if (zoneValue != "NA" && zoneValue != "") {
        updateCell("Zone " + zoneName, zoneValue);
      }

      // Respond with zone value for zones with Nexa(s), sheet -> device
      if (zoneValue != "NA") {
        zoneValue = readCell("Zone " + zoneName, 0);
        response["zone." + zoneName] = zoneValue;
      }

      // Log set value for zones with sensor and Nexa(s)
      if (zoneValue != "NA" && zoneTemp != "") {
        headerValues.push(zoneName + " !");
        row.push(zoneValue);
      }

      // Log duty cycle if reported by device
      if (zoneDutyCycle != "") {
        updateCell("Zone " + zoneName + " %", zoneDutyCycle);
        headerValues.push(zoneName + " %");
        row.push(zoneDutyCycle);
      }

      // Log temp if reported by device
      if (zoneTemp != "") {
        zoneTemp = parseFloat(zoneTemp);
        updateCell("Zone " + zoneName + " °C", zoneTemp);
        headerValues.push(zoneName + " °C");
        row.push(zoneTemp);
      }
    }
  });

  // Log temperatures
  var logName = Utilities.formatString("%04d-%02d", timestamp.getFullYear(), timestamp.getMonth()+1)
  var logSheet = getSheet(logName, 1);
  logSheet.getRange(1, 1, 1, headerValues.length)
          .setValues([headerValues])
          .setFontWeight("bold");
  logSheet.appendRow(row)
          .autoResizeColumns(1, headerValues.length);

  return ContentService
    .createTextOutput(JSON.stringify(response))
    .setMimeType(ContentService.MimeType.JSON);
}

function getParam(e, name) {
  var params = getParams(e, name);
  if (params.length > 0) {
    return params[0];
  }
  return null;
}

function getParams(e, name) {
  if (e != null && e.parameters != null) {
    if (e.parameters[name] != null) {
      return e.parameters[name];
    }
  }
  return [];
}

function readCell(key, defaultValue) {
  cell = findCell(key);
  if (cell != null) {
    return cell.getValue();
  }
  else {
    getConfigSheet().appendRow([key, defaultValue])
    return defaultValue;
  }
}

function updateCell(key, value) {
  cell = findCell(key);
  if (cell != null) {
    return cell.setValue(value);
  }
  else {
    getConfigSheet().appendRow([key, value])
    return findCell(key);
  }
}

function findCell(key) {
  var sheet = getConfigSheet();
  for (var row = 1; row <= 20; row++) {
    var r = sheet.getRange(row, 1);
    if (r.getValue() == key) {
      return sheet.getRange(row, 2);
    }
  }
  return null;
}

function getConfigSheet() {
  return getSheet("config", 0);
}

function getSheet(name, insertIndex) {
  var sheet = SpreadsheetApp.getActive().getSheetByName(name);
  if (sheet == null) {
    sheet = SpreadsheetApp.getActive().insertSheet(name, insertIndex)
    sheet.deleteRows(1, sheet.getMaxRows() - 4);
    sheet.deleteColumns(1, sheet.getMaxColumns() - 2);
  }
  return sheet;
}

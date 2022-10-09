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

  // Handle scheduled temp change
  var scheduleTime = new Date(readCell("Schedule Time", ""));
  var scheduleZone = readCell("Schedule Zone", "");
  var scheduleTemp = readCell("Schedule Value", "");
  if (timestamp > scheduleTime && scheduleZone != "" && scheduleTemp > 0) {
    //console.log("Schedule " + scheduleTemp + " in " + scheduleZone + " at " + scheduleTime);
    updateCell("Zone "+scheduleZone, scheduleTemp);
    updateCell("Schedule Time", "");
    updateCell("Schedule Zone", "");
    updateCell("Schedule Value", "");
  }

  // Prepare to log temperatures
  var headerValues = ["Time"];
  var row = [timestamp];

  // Sync zone settings and temperatures
  getParams(e, "zone").forEach(it => {
    // name;type(A/N/S);temp;value
    // A (Auto):   Both sensor and Nexa -> report temp, device value if reverse sync
    // S (Sensor): Only sensor, no Nexa -> report temp, no device value
    // M (Manual): No senor, only Nexa  -> no temp, no device value
    let zone = it.split(";");
    if (zone.length >= 5) {
      let zoneName  = zone[0];
      let zoneType  = zone[1];
      let zoneValue = zone[2];
      let zoneTemp  = parseFloat(zone[3]);
      let zoneDutyCycle = parseFloat(zone[4]);

      if (zoneType=="A" && zoneValue != -1) {
        // Reverse sync for auto-zones, device -> sheet
        // Manual zones may use a formula, so don't overwrite it
        updateCell("Zone "+zoneName, zoneValue);
      }
      if (zoneType=="A" || zoneType=="M") {
        // Sync zone value, sheet -> device
        // Manual zones may use a formula
        zoneValue = readCell("Zone "+zoneName, 0);
        response["zone."+zoneName] = zoneValue;
      }
      if (zoneType=="A") {
        // Log set value only for auto-zone
        headerValues.push(zoneName + " !");
        row.push(zoneValue);
        // Duty cycle only relevant for auto-zone
        updateCell("Zone "+zoneName+" %", zoneDutyCycle);
        headerValues.push(zoneName + " %");
        row.push(zoneDutyCycle);
      }
      if (zoneType=="A" || zoneType=="S") {
        // Log temp
        updateCell("Zone "+zoneName+" °C", zoneTemp);
        headerValues.push(zoneName+" °C");
        row.push(zoneTemp);
      }
    }
  });

  // Log temperatures
  var logName = Utilities.formatString("_%04d-%02d", timestamp.getFullYear(), timestamp.getMonth()+1)
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
    getStatusSheet().appendRow([key, defaultValue])
    return defaultValue;
  }
}

function updateCell(key, value) {
  cell = findCell(key);
  if (cell != null) {
    return cell.setValue(value);
  }
  else {
    getStatusSheet().appendRow([key, value])
    return findCell(key);
  }
}

function findCell(key) {
  var sheet = getStatusSheet();
  for (var row = 1; row <= 20; row++) {
    var r = sheet.getRange(row, 1);
    if (r.getValue() == key) {
      return sheet.getRange(row, 2);
    }
  }
  return null;
}

function getStatusSheet() {
  return getSheet("_status", 0);
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

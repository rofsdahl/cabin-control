function doGet(e) {

  var timestamp = new Date();
  updateCell("Last sync", timestamp)
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

  updateCell("Errors", getParam(e, "errCount"));

  var response = {}
  response["syncInterval"] = readCell("Sync interval", 60);

  // Scheduled temp change?
  var scheduleTime = new Date(readCell("Schedule time", ""));
  var scheduleTemp = readCell("Schedule temp", "");
  if (timestamp > scheduleTime && scheduleTemp > 0) {
    //console.log("Schedule temp change " + scheduleTemp + " at " + scheduleTime);
    updateCell("Set temp", scheduleTemp);
    updateCell("Schedule time", "");
    updateCell("Schedule temp", "");
  }

  // Sync setTemp both ways
  var setTemp = getParam(e, "setTemp");
  if (setTemp != null) {
    // New setTemp from device (reverse sync)
    updateCell("Set temp", setTemp);
  }
  else {
    setTemp = readCell("Set temp", 10);
  }
  response["setTemp"] = setTemp;

  // Log temperatures
  var headerValues = ["Time", "Set temp"];
  var row = [timestamp, setTemp];
  getParams(e, "temp").forEach(it => {
    let v = it.split(":");
    if (v.length > 1) {
      let name = v[0];
      let value = parseFloat(v[1]);
      updateCell("Temp "+name, value);
      headerValues.push(name);
      row.push(value);
    }
  });

  var tempSheetName = Utilities.formatString("%04d-%02d", timestamp.getFullYear(), timestamp.getMonth()+1)
  var tempSheet = getSheet(tempSheetName, 1);
  tempSheet.getRange(1, 1, 1, headerValues.length)
           .setValues([headerValues])
           .setFontWeight("bold");
  tempSheet.appendRow(row)
           .autoResizeColumns(1, headerValues.length);

  // Update status for extra outputs
  getParams(e, "output").forEach(it => response["output."+it] = readCell("Output "+it, 0));

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

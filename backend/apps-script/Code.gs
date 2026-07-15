function doPost(e) {
  var data = JSON.parse(e.postData.contents);
  var action = data.action || 'log_reading';
  if (action === 'log_reading') {
    var sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName('Sheet1');
    sheet.appendRow([
      new Date(),
      data.fault_class || '',
      data.temp_c || '',
      data.rpm || '',
      data.note || ''
    ]);
    return jsonResponse({ status: 'ok' });
  }
  if (action === 'update_live') {
    var live = SpreadsheetApp.getActiveSpreadsheet().getSheetByName('Live');
    live.appendRow([
      new Date(),
      data.fault_class || '',
      orBlank(data.confidence),
      orBlank(data.healthy),
      orBlank(data.imbalance),
      orBlank(data.obstruction),
      orBlank(data.temp_c)
    ]);
    if (live.getLastRow() > 101) {
      live.deleteRow(2);
    }
    return jsonResponse({ status: 'ok' });
  }
  if (action === 'request_explanation') {
    var control = SpreadsheetApp.getActiveSpreadsheet().getSheetByName('Control');
    control.getRange('A2').setValue(true);
    return jsonResponse({ status: 'ok' });
  }
  if (action === 'submit_explanation') {
    var control = SpreadsheetApp.getActiveSpreadsheet().getSheetByName('Control');
    control.getRange('B2').setValue(data.explanation || '');
    control.getRange('A2').setValue(false);
    return jsonResponse({ status: 'ok' });
  }
  return jsonResponse({ status: 'error', message: 'unknown action' });
}
function doGet(e) {
  var action = e.parameter.action;
  if (action === 'check_trigger') {
    var control = SpreadsheetApp.getActiveSpreadsheet().getSheetByName('Control');
    var trigger = control.getRange('A2').getValue();
    return jsonResponse({ trigger: trigger === true });
  }
  if (action === 'get_explanation') {
    var control = SpreadsheetApp.getActiveSpreadsheet().getSheetByName('Control');
    var explanation = control.getRange('B2').getValue();
    return jsonResponse({ explanation: explanation || '' });
  }
  if (action === 'get_live') {
    var live = SpreadsheetApp.getActiveSpreadsheet().getSheetByName('Live');
    var rows = live.getDataRange().getValues();
    return jsonResponse(rows);
  }
  // Default (no action param): existing behavior, dashboard log fetch
  var sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName('Sheet1');
  var rows = sheet.getDataRange().getValues();
  return jsonResponse(rows);
}
function orBlank(v) {
  return (v === undefined || v === null) ? '' : v;
}
function jsonResponse(obj) {
  return ContentService.createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}

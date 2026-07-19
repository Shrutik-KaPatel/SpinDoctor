function doPost(e) {
  try {
    var data = JSON.parse(e.postData.contents);
    var action = data.action || 'log_reading';

    if (action === 'log_reading') {
      var sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName('Sheet1');
      sheet.appendRow([
        new Date(),
        data.fault_class || '',
        data.temp_c || '',
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
  } catch (err) {
    // Never let an exception escape as Google's raw HTML error page,
    // that response carries no CORS header and gets silently blocked
    // by the browser, which looks exactly like a random CORS failure
    // from the dashboard's side even though the real cause is a
    // server-side exception on this specific call.
    return jsonResponse({ status: 'error', message: String(err) });
  }
}

function doGet(e) {
  var action = e.parameter.action;
  try {
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
  } catch (err) {
    // get_live/default expect an array of rows on the front end, so
    // fail into that same shape (a harmless one-row "error" table the
    // dashboard's own malformed-row filter already discards cleanly),
    // rather than an object shape those two call sites don't expect.
    if (action === 'get_live' || !action) {
      return jsonResponse([['error'], [String(err)]]);
    }
    return jsonResponse({ status: 'error', message: String(err) });
  }
}

function orBlank(v) {
  return (v === undefined || v === null) ? '' : v;
}

function jsonResponse(obj) {
  return ContentService.createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}

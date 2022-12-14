function parseQuery(request, key) {
  var params = request.queryString.split("&");
  for (var j = 0; j < params.length; ++j) {
    var p = params[j];
    if (p == key) {
      return true;
    }
    if (p.indexOf(key + "=") == 0) {
      return p.substring(key.length + 1);
    }
    if (!p.includes("=") && key == "") {
      return p;
    }
  }
  return false;
}

function handleRequest(request, response) {
  var referer = request.hasHeader("Referer")
    ? request.getHeader("Referer")
    : undefined;
  if (
    referer == "http://mochi.test:8888/tests/dom/media/test/test_referer.html"
  ) {
    var name = parseQuery(request, "name");
    var type = parseQuery(request, "type");
    var file = Services.dirsvc.get("CurWorkD", Ci.nsIFile);
    var fis = Cc["@mozilla.org/network/file-input-stream;1"].createInstance(
      Ci.nsIFileInputStream
    );
    var bis = Cc["@mozilla.org/binaryinputstream;1"].createInstance(
      Ci.nsIBinaryInputStream
    );
    var paths = "tests/dom/media/test/" + name;
    var split = paths.split("/");
    for (var i = 0; i < split.length; ++i) {
      file.append(split[i]);
    }
    fis.init(file, -1, -1, false);
    bis.setInputStream(fis);
    var bytes = bis.readBytes(bis.available());
    response.setHeader("Content-Length", "" + bytes.length, false);
    response.setHeader("Content-Type", type, false);
    response.write(bytes, bytes.length);
    bis.close();
  } else {
    response.setStatusLine(request.httpVersion, 404, "Not found");
  }
}

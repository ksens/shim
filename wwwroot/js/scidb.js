var x = -1;             // Session ID
var cancelled = false;  // cancel flag

var FILE;


// This fixes a nasty IE 8 bug (by appending changing query parameters):
$.ajaxSetup({
    cache: false
});

function cancel()
{
  cancelled = true;
  $("#result")[0].innerHTML = "<pre>Query cancellation in process...</pre>";
  $.get("/cancel?id="+x);
  $("#querycontainer").spin(false);
}

// val must be true/false
function lockstate(val)
{
  if(val)
  {
    $("#result")[0].innerHTML = "<pre>Wait...</pre>";
    $("#querycontainer").spin();
    $("#exq").addClass("disabled");
    $("#exqn").addClass("disabled");
    $("#uplo").addClass("disabled");
    $("#can").removeClass("disabled");
  } else {
    $("#querycontainer").spin(false);
    $("#exq").removeClass("disabled");
    $("#exqn").removeClass("disabled");
    $("#uplo").removeClass("disabled");
    $("#can").addClass("disabled");
  }
}

function execute_query(ret)
{
  cancelled=false;
  lockstate(true);

$.get(
  "/new_session",
  function(data){
    x = parseInt(data); // session ID
    var sq = $("#query")[0].value;
    var q = encodeURIComponent(sq);
    var urix = "/execute_query?id="+x+"&query="+q+"&save=dcsv";
    if(ret==0)
      urix = "/execute_query?id="+x+"&query="+q;
    var urir = "/read_lines?id="+x+"&n="+numlines.value;
    var rel = "/release_session?id="+x;

    $.get(urix)
     .fail(function(z){$("#result")[0].innerHTML = "<pre>" +
        z.responseText.replace(">","&gt;").replace("<","&lt;")
        + "</pre>";
        lockstate(false);
     })
     .done(function(z)
     {
       if(ret==0)
       {
         $("#result")[0].innerHTML = "<pre>SciDB Query ID " + z + " complete.</pre>";
         $.get(rel);
       } else
       {
// Read the output
         $.get(urir,
           function(z)
           {
             var gt = />/g;
             var lt = /</g;
             $("#result")[0].innerHTML = "<pre>" + 
               z.replace(gt,"&gt;").replace(lt,"&lt;") + "</pre>";
             lockstate(false);
           }).always(function(z){$.get(rel);});
       }
         lockstate(false);
     })

  })
  .fail(function()
  {
    $("#result")[0].innerHTML = "SESSION ERROR!";
    lockstate(false);
  });
}

function csv2scidb(csv, chunksize, start)
{
  var chunk = 0;     // chunk counter
  var buf   = csv.split("\n");
  var i = 0;
  for(var j=0;j < buf.length - 1;j++)
  {
    tmp = buf[j];
    if( (j % chunksize) == 0)
    {
      tmp = "{" + (start + chunk*chunksize) + "} [\n(" + tmp + "),";
      buf[j] = tmp;
      chunk += 1;
    }
    else if( ((j+1) % chunksize) == 0)
    {
      tmp = "(" + tmp + ")];";
      buf[j] = tmp;
    }
    else
    {
      tmp = "(" + tmp + "),";
      buf[j] = tmp;
    }
  }
  j = buf.length - 1;
  tmp = buf[j];
// Might have trailing \n...
  if(tmp.length<1) {return(buf.join("\n"))}
  if( (j % chunksize) == 0)
  {
    tmp = "{" + (start + chunk*chunksize) + "} [\n(" + tmp + ")];";
    buf[j] = tmp;
  }
  else
  {
    tmp = "(" + tmp + ")];";
    buf[j] = tmp;
  }
  return (buf.join("\n"));
}

function do_upload()
{
  var gt = />/g;
  var lt = /</g;
// Figure out chunking and run csv2scidb
  var schema = $("#arraySchema").val();
  var dims = $("#arraySchema").val().replace(/.*=/,"").replace(/]/,"").replace(/:/,",").split(",");
  var start = parseInt(dims[0]);
  var chunkSize = parseInt(dims[2]);
  var CSV = csv2scidb(FILE, chunkSize, start);
//  var CSV = csv2scidb(FILE.replace(/\r/g,""), chunkSize, start);
  
  $.get(
    "/new_session",
    function(data){
      lockstate(true);
      x = parseInt(data); // session ID
      var rel = "/release_session?id="+x;
      var urix = "/upload_file?id="+x

      var boundary = "--d1f47951faa4";
      var body = '--' + boundary + '\r\n'
               + 'Content-Disposition: form-data; name="file"; '
               + 'filename="data.csv"\r\n'
               + 'Content-Type: application/octet-stream\r\n\r\n'
               + CSV + '\r\n'
               + '--' + boundary + '--';
      $.ajax({
        contentType: "multipart/form-data; boundary="+boundary,
        data: body,
        type: "POST",
        url: urix
      }).done( function (data) {
          var fn = data.replace(/^[\r\n]+|\.|[\r\n]+$/g, "");
          var arrayName = $("#arrayName").val();
          var query = "store(input("+schema+",'"+fn+"',0),"+arrayName+")";
          var q = encodeURIComponent(query);
          var urir = "/execute_query?id="+x+"&query="+q+"&release=1";
          $.get(urir, function(z) {
                   $("#query")[0].value = "scan(" + arrayName + ")";
                   $("#result")[0].innerHTML = "<pre>OK</pre>";
                   execute_query(1);
                 }).fail(function(z) {
                 $("#result")[0].innerHTML = "<pre>" +
                 z.responseText.replace(">","&gt;").replace("<","&lt;")
                 + "</pre>";
                 $.get(rel);
             }).always(function(z){
                lockstate(false);
             });
      }).fail(function(z){$.get(rel); lockstate(false);});
    }).always(function(z){lockstate(false)});
}

function handleFileSelect(evt) {
  var files = evt.target.files; // FileList object
  var f = files[0];
  var reader = new FileReader();
  // Closure to capture the file information.
  reader.onload = (function(theFile) {
    return function(e) {
        FILE = e.target.result.replace(/\r/g,""); // Windows
        var firstline = FILE.substring(0,FILE.indexOf("\n"));
        if($("#header").prop("checked"))
        {
          FILE = FILE.substring(FILE.indexOf("\n")+1,FILE.length);
        }
        var m = (FILE.match(/\n/g)||[]).length;  // lines
        var n = firstline.split(",").length;     // attributes
        var attr = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ".split("");
        if($("#header").prop("checked"))
        {
          attr = firstline.split(",");  // header for attribute names
        }
        var schema = "<";
        for(var i=0;i < Math.min(n-1,52); i++)
        {
          schema = schema + attr[i] + ":string, ";
        }
        schema = schema + attr[i] + ":string>";
        schema = schema + "[row=0:" + (m-1) + "," + Math.min(m,1000) + ",0]";
        $("#arraySchema").val(schema);
    }
  })(f);

  reader.readAsText(f);
}



$(document).ready(function()
  {
    $("#query").focus();
    $('input[id=fup]').change(function() {
      $('#pretty-input').val($(this).val().replace("C:\\fakepath\\", ""));
    });
    $("#can").addClass("disabled");
    document.getElementById('fup').addEventListener('change', handleFileSelect, false);
  });

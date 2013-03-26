var x = -1;             // Session ID
var cancelled = false;  // cancel flag

var FILE;
var DATA;


// This fixes a notorious IE 8 bug:
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

function execute_query(ret)
{
  cancelled=false;
  $("#exq")[0].disabled=true;
  $("#exqn")[0].disabled=true;
  $("#result")[0].innerHTML = "<pre>Wait...</pre>";
  $("#can")[0].disabled=false;
  $("#querycontainer").spin();

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
        $("#exq")[0].disabled=false;
        $("#exqn")[0].disabled=false;
        $("#can")[0].disabled=true;
        $("#querycontainer").spin(false);
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
             $("#exq")[0].disabled=false;
             $("#exqn")[0].disabled=false;
             $("#can")[0].disabled=true;
             $("#querycontainer").spin(false);
           }).always(function(z){$.get(rel);});
       }
         $("#exq")[0].disabled=false;
         $("#exqn")[0].disabled=false;
         $("#can")[0].disabled=true;
         $("#querycontainer").spin(false);
     })

  })
  .fail(function()
  {
    $("#result")[0].innerHTML = "SESSION ERROR!";
    $("#exq")[0].disabled=false;
    $("#exqn")[0].disabled=false;
    $("#can")[0].disabled=true;
    $("#querycontainer").spin(false);
  });
}

function csv2scidb(csv, chunksize)
{
  var chunk = 0;     // chunk counter
  var buf   = csv.split("\n");
  var i = 0;
  for(var j=0;j < buf.length - 1;j++)
  {
    tmp = buf[j];
    if( (j % chunksize) == 0)
    {
      tmp = "{" + chunk + "} [\n(" + tmp + "),";
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
    tmp = "{" + chunk + "} [\n(" + tmp + ")];";
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
  $("#fup").show();
}

function handleFileSelect(evt) {
  $("#fup").hide();
  var files = evt.target.files; // FileList object
  var f = files[0];
  var reader = new FileReader();

  // Closure to capture the file information.
  reader.onload = (function(theFile) {
    return function(e) {
        FILE=csv2scidb(e.target.result,10);
        $.get(
          "/new_session",
          function(data){
            x = parseInt(data); // session ID
            var rel = "/release_session?id="+x;
alert(x);
            var urix = "/upload_file?id="+x
            $.post(urix,
              function(data){
alert(data);
                var q = encodeURIComponent("load('"+data+"')");
                var urir = "/execute_query?id="+x+"&query="+q;
                $.get(urir).always(function(z){$.get(rel);});
              });
          });
    }
  })(f);

  // Read in the image file as a data URL.
  reader.readAsText(f);
}



$(document).ready(function()
  {
    $("#query").focus();
    $("#fup").hide();
    document.getElementById('fup').addEventListener('change', handleFileSelect, false);
  });

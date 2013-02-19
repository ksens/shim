var x=-1;             // Session ID
var cancelled = false;  // cancel flag
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

$(document).ready(function(){$("#query").focus();});

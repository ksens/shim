// XXX Right now we just assume port is always 1239. We need to parse
// the config.ini file for a list of valid ports associated with each SciDB
// cluster ID. XXX Add this...
  var ports=1239;
  var DEBUG;

// This fixes a notorious IE 8 bug:
$.ajaxSetup({
    cache: false
});

  dostartstop = function(x)
  {
    $("#stopbtn").attr("disabled","disabled")
    $("#startbttn").attr("disabled","disabled")
    $("#bgetlog").attr("disabled","disabled")
//    $("#beditconfig").attr("disabled","disabled")
    $("#scidb_dash").spin();
    $.get(x, function(z){location.reload();}).fail(
             function(z){location.reload();});
  }


  getlog = function()
  {
    $("#bgetlog").attr("disabled","disabled")
//    $("#beditconfig").attr("disabled","disabled")
    $("#configini").show();
    $("#scidb_dash").spin();


    $.get( "/get_log",
      function(data){
        $("#configini").val(data);
        $("#configini").show();
        $("#closebtn").show();
        $("#scidb_dash").spin(false);
      }
    ).fail(function(z){
        $("#bgetlog").attr("disabled",false);
        $("#configini").hide();
        $("#closebtn").hide();
//            $("#beditconfig").attr("disabled",false)
        $("#scidb_dash").spin(false);
    });
  }
  doedit = function()
  {
//    $("#beditconfig").attr("disabled","disabled")
    $("#bgetlog").attr("disabled","disabled")
    $.get( "/get_config",
      function(data){
        $("#configini").val(data);
        $("#configini").show();
        $("#savebtn").show();
        $("#closebtn").show();
      }
    ).fail(function(z){
            $("#bgetlog").attr("disabled",false);
//            $("#beditconfig").attr("disabled",false)
            $("#scidb_dash").spin(false);
    });
  }
  doclose = function()
  {
     $("#configini").val("");
     $("#configini").hide();
     $("#savebtn").hide();
     $("#closebtn").hide();
     $("#bgetlog").attr("disabled",false)
//     $("#beditconfig").attr("disabled",false)
  }

  $(document).ready(function()
  {

    $("#configini").hide();
    $("#savebtn").hide();
    $("#closebtn").hide();
// Parse the config.ini file for available SciDB configuration names
//    $.get( "/get_config",
//      function(data){
//        clusters=jQuery.grep(data.split("\n"), function(n,i) {return(n.match(/\[.*\]/));})
//        $.each(clusters, function(i,v)
//          {
//            w = v.replace("[","").replace("]","");
//            $("#startmenu").append("<li><a href='#'>"+w+"</a></li>");
//            $("#stopmenu").append("<li><a href='#'>"+w+"</a></li>");
//          });
//
//          $("#startmenu li").click(function() {
//            dostartstop("/start_scidb?db="+$(this).text());
//          });
//          $("#stopmenu li").click(function() {
//            dostartstop("/stop_scidb?db="+$(this).text());
//          });
//      }
//    );
    hello("list('instances')",100);

  });

function hello(sq, numlines)
{
var result="";
$.get(
  "/new_session",
  function(data){
    x = parseInt(data); // session ID
    var q = encodeURIComponent(sq);
    var urix = "/execute_query?id="+x+"&query="+q+"&save=dcsv";
    var urir = "/read_lines?id="+x+"&n="+numlines;
    var rel = "/release_session?id="+x;

    $.get(urix)
     .done(function(z)
      {
        $.get(urir,
          function(z)
          {
            var gt = />/g;
            var lt = /</g;
            result = result + z.replace(gt,"&gt;").replace(lt,"&lt;");
            var s = "<h2 class='allgreen'>Connected to SciDB host " + location.hostname + "</h2>"
//            s = s + "(Operational since "+result.split("\n")[1].split(",")[3].replace(/"/g,"")+")";
            s = s + "<h3>SciDB instances in this cluster:</h3>"
            s = s + "<div class='instances'>";
            $.each(result.split("\n"),function(i,v)
            {
              if(i>0 && v.length>0)
              {
                x = v.split(",");
                s = s+x[0].match(/".*"/)[0].replace(/"/g,"")+" "+x[1] + "<br/>";
              }
            });
            s = s + "</div>";
            $("#scidb_dash")[0].innerHTML = s;
            $.get(rel);
          })
      }).fail(function(z)
             {
               $("#scidb_dash")[0].innerHTML="<h2>SciDB Connection Error</h2><h3>Try starting a SciDB cluster</h3>";
               $.get(rel);
      });
  });
}

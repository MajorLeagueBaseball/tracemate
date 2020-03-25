var hist = CirconusHistogram({ minbarwidth: 5, width: 600, height: 200, yaxis: true, xaxis: true, hudlegend: true });
var stop_ui = false;
var local_skew = 0;
var check_search = "";
var version = {};
var startTime = +new Date();
var tm_api_base = "/";
function refresh_version() {
  if(stop_ui) return;
  $.ajax(tm_api_base + "version.json").done(function (x) {
    version = x;

    $("#tm-version").text(version.version);
  });
}
refresh_version();

function pretty_mb(mb) {
  var d = function(a) { return '<span class="text-muted">'+a+'</span>'; };
  if(mb < 1) return (mb / 1024).toFixed(0) + d("kb");
  if(mb > 1024) return (mb / 1024).toFixed(0) + d("Gb");
  if(mb > 1024*1024) return (mb / (1024*1024)).toFixed(0) + d("Tb");
  return parseFloat(mb).toFixed(0) + d("Mb");
}
function nice_date(s) {
  var d = new Date(s);
  var year = d.getFullYear();
  var month = d.getMonth();
  var day = d.getDate();
  var hour = d.getHours();
  var minute = d.getMinutes();
  var second = d.getSeconds();
  return year + "/" +
         ((month < 10) ? "0" : "") + month + "/" +
         ((day < 10) ? "0" : "") + day + " " +
         ((hour < 10) ? "0" : "") + hour + ":" +
         ((minute < 10) ? "0" : "") + minute + ":" +
         ((second < 10) ? "0" : "") + second;
}
function nice_time(s) {
  var negative = (s < 0);
  var days, hours, minutes, seconds;
  s = Math.abs(s);
  days = Math.floor(s/86400); s -= (86400*days);
  hours = Math.floor(s/3600); s -= (3600*hours);
  minutes = Math.floor(s/60); s -= (60*minutes);
  var time = "";
  if(days) time = time + days + "d"
  if(hours) time = time + hours + "h"
  if(minutes) time = time + minutes + "m"
  if(s || time === "") {
    if(s == Math.floor(s))
      time = time + s + "s"
    else
      time = time + parseFloat(s).toFixed(3) + "s"
  }
  if(negative) time = time + " ago";
  return time;
}

function $badge(n) {
  return $("<span class=\"tag tag-pill tag-default\"/>").text(n);
}
function $modalButton(n, id) {
  return $('<button type="button" class="tag tag-primary" data-toggle="modal" data-target="#' + id + '"/>').html(n);
}
function $label(n, type) {
  if(!type) type = "default";
  return $("<span class=\"tag tag-"+type+"\"/>").text(n);
}
function $nice_type(t) {
  if(t == "s") t = "string";
  else if(t == "i") t = "int32";
  else if(t == "I") t = "uint32";
  else if(t == "l") t = "int64";
  else if(t == "L") t = "uint64";
  else if(t == "n") t = "double";
  return $label(t, "info");
}
function $flags(which, flag) {
  var $d = $("<div/>");
  if(which & 1) {
    if(flag & 0x1) $d.append($label("running","primary"));
    if(flag & 0x2) $d.append($label("killed","danger"));
    if(flag & 0x4) $d.append($label("disabled","default"));
    if(flag & 0x8) $d.append($label("unconfig","warning"));
  }
  if(which & 2) {
    if(flag & 0x10) $d.append($label("T","success"));
    if(flag & 0x40) $d.append($label("R","info"));
    else if(flag & 0x20) $d.append($label("R?","info"));
    if(flag & 0x1000) $d.append($label("-S","warning"));
    if(flag & 0x2000) $d.append($label("-M","warning"));
    if(flag & 0x4000) {
      if(flag & 0x8000) $d.append($label("v6","default"));
      else $d.append($label("v6,v4","default"));
    } else {
      if(flag & 0x8000) $d.append($label("v4","default"));
      //else $d.append($label("v4,v6","default"));
    }
    if(flag & 0x00010000) $d.append($label("P", "info"));
  }
  return $d;
}
var active_histogram = null;
function displayHistogram(name, data, subname) {
  var d = {}
  if (data && data._value && Array.isArray(data._value)) {
    for(var i=0;i<data._value.length;i++) {
      var bin = data._value[i]
      var eidx = bin.indexOf('=');
      if(eidx > 0) {
        d[bin.substring(2,eidx-1)] = parseInt(bin.substring(eidx+1));
      }
    }
  }
  $("span#histogram-name").text(name + " " + subname);
  $("#modal-histogram").empty();
  hist.render("#modal-histogram", d);
}
function $hist_button(name, stats, subname) {
  if(stats.hasOwnProperty(name)) {
    var $perf = $modalButton(" ", "histModal").on('click', function() {
      var v = stats[name];
      if(subname) v = v[subname];
      displayHistogram(name, v, subname || "");
    });
    $perf.addClass("glyphicon");
    $perf.addClass("glyphicon-stats");
    return $perf;
  }
  return null;
}
function mk_jobq_row(jobq,detail) {
  var $tr = $("<tr/>");
  $tr.append($("<td/>").html($badge(detail.backlog+detail.inflight)));
  var $cb = $("<span/>").text(jobq);
  $tr.append($("<td/>").html($cb));
  var $conc = $("<td class=\"text-center\"/>");
  if(detail.desired_concurrency == 0)
    $conc.text("N/A");
  else if(detail.concurrency == detail.desired_concurrency)
    $conc.text(detail.concurrency);
  else
    $conc.html(detail.concurrency + " &rarr; " + detail.desired_concurrency);
  $tr.append($conc);
  $tr.append($("<td class=\"text-right\"/>").append($badge(detail.total_jobs)));
  $cb = $("<small/>").text(parseFloat(detail.avg_wait_ms).toFixed(3) + "ms");
  var $perf, $td;
  $td = $("<td class=\"text-right\"/>");
  $perf = $hist_button(jobq, eventer_stats.jobq, "wait");
  if($perf) $td.append($perf);
  $td.append($cb);
  $tr.append($td);
  $tr.append($("<td class=\"text-left\"/>").append($badge(detail.backlog)));
  $cb = $("<small/>").text(parseFloat(detail.avg_run_ms).toFixed(3) + "ms");
  $perf = $hist_button(jobq, eventer_stats.jobq, "latency");
  $td = $("<td class=\"text-right\"/>");
  if($perf) $td.append($perf);
  $td.append($cb);
  $tr.append($td);
  $tr.append($("<td class=\"text-left\"/>").append($badge(detail.inflight)));
  return $tr;
}
function mk_timer_row(event) {
  var $tr = $("<tr/>");
  var $cb = $("<span/>").text(event.callback);
  var $perf = $hist_button(event.callback, eventer_stats.callbacks);
  var $td = $("<td/>");
  if ($perf !== null) { $td.append($perf); }
  $td.append($cb)
  $tr.append($td);
  $tr.append($("<td/>").text(new Date(event.whence)));
  return $tr;
}
function mk_socket_row(event) {
  var $tr = $("<tr/>");
  var mask = [];
  if(event.mask & 1) mask.push("R");
  if(event.mask & 2) mask.push("W");
  if(event.mask & 4) mask.push("E");
  $tr.append($("<td/>").html(event.fd + "&nbsp").append($badge(mask.join("|")).addClass('pull-right')));
  $tr.append($("<td/>").html('<small>'+event.impl+'</small>'));
  var $td = $("<td/>");
  var $cb = $("<span/>").text(event.callback);
  var $perf = $hist_button(event.callback, eventer_stats.callbacks);
  if ($perf !== null) { $td.append($perf); }
  $td.append($cb);
  $tr.append($td);
  $tr.append($("<td/>").text(event.local ? event.local.address+":"+event.local.port : "-"));
  $tr.append($("<td/>").text(event.remote ? event.remote.address+":"+event.remote.port : "-"));
  return $tr;
}

function update_eventer(uri, id, mkrow) {
  return function(force) {
    if(stop_ui) return;
    var $table = $("div#"+id+" table" + (force ? "" : ":visible"));
    if($table.length == 0) return;
    var st = jQuery.ajax(uri);
    st.done(function( events ) {
      var $tbody = $("<tbody/>");
      if(events.hasOwnProperty('length')) {
        events.forEach(function(event) {
          $tbody.append(mkrow(event));
        });
      } else {
        var keys = [];
        for(var name in events) keys.push(name);
        keys.sort().forEach(function(name) {
          $tbody.append(mkrow(name, events[name]));
        });
      }
      $table.find("tbody").replaceWith($tbody);
    });
  };
}

function startInternals() {
  setInterval(update_eventer(tm_api_base + "eventer/sockets.json",
                             "eventer-sockets", mk_socket_row),
              5000);
  $('#eventer-sockets').on('shown.bs.collapse', function () {
    update_eventer(tm_api_base + "eventer/sockets.json",
                   "eventer-sockets", mk_socket_row)();
  });
  update_eventer(tm_api_base + "eventer/sockets.json",
                 "eventer-sockets", mk_socket_row)(true);
  
  setInterval(update_eventer(tm_api_base + "eventer/timers.json",
                             "eventer-timers", mk_timer_row),
              5000);
  $('#eventer-timers').on('shown.bs.collapse', function () {
    update_eventer(tm_api_base + "eventer/timers.json",
                   "eventer-timers", mk_timer_row)();
  });
  setInterval(update_eventer(tm_api_base + "eventer/jobq.json",
                             "eventer-jobq", mk_jobq_row),
              5000);
  $('#eventer-jobq').on('shown.bs.collapse', function () {
    update_eventer(tm_api_base + "eventer/jobq.json",
                   "eventer-jobq", mk_jobq_row)();
  });
}


var last_log_idx;
function refresh_logs(force) {
  if(stop_ui) return;
  var qs = "";
  var $c = $("#main-log-window");
  if(typeof(last_log_idx) !== 'undefined')
    qs = "?since=" + last_log_idx;
  else
    qs = "?last=100";
  $.ajax(tm_api_base + "eventer/logs/internal.json" + qs).done(function (logs) {
    var atend = force || Math.abs(($c[0].scrollTop + $c[0].clientHeight - $c[0].scrollHeight));
    for(var i in logs) {
      var log = logs[i];
      $row = $("<div class=\"row\"/>");
      $row.append($("<div class=\"col-md-2 text-muted\"/>").text(nice_date(log.whence)));
      $row.append($("<div class=\"col-md-10\"/>").text(log.line));
      $c.append($row);
      if(log.idx < last_log_idx) refresh_capa();
      last_log_idx = log.idx;
      if(atend < 20) {
        $c[0].scrollTop = $c[0].scrollHeight;
        $c[0].scrollLeft = 0;
      }
    }
    var rows = $c.find("> div");
    var cnt = 0;
    for(var i = rows.length ; i > 1000; i--) 
      rows[cnt++].remove();
  });
}
refresh_logs(1);
setInterval(refresh_logs, 1000);

var eventer_stats = { callbacks: {}, jobq: {}};
function refresh_stats(cb) {
  if(stop_ui) return;
  $.ajax(tm_api_base + "mtev/stats.json").done(function (stats) {
    eventer_stats = stats.mtev.eventer;
    if(cb) cb();
  });
}
refresh_stats(startInternals)
setInterval(refresh_stats, 5000);

$(document).ready(function() {
  $('.navbar-nav a.nav-link').click(function (e) {
    $(this).tab('show');
    var scrollmem = $('body').scrollTop();
    window.location.hash = this.hash;
    $('html,body').scrollTop(scrollmem);
  });

  var hash = window.location.hash.substring(1);
  if(!hash) return;
  var nav = hash.split(/:/);

  if(nav[0]) $('ul.nav a[href="#' + nav[0] + '"]').tab('show');
});

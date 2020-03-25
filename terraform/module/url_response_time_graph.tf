resource "circonus_graph" "url_response_time_graph" {
  count       = length(var.urls)
  name        = "${lookup(var.urls[count.index],"url")} - Average Response Time"
  description = "Average response time for this URL across all hosts"
  notes       = ""
  graph_style = "area"
  line_style  = "interpolated"

  metric {
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "db - average_latency - ${lookup(var.urls[count.index],"url")}|ST[service:${lookup(var.urls[count.index],"service")},host:all,ip:all]"
    name           = "Average DB Latency (ms)"
    axis           = "left"
    alpha          = 0.8
    stack          = 0
    formula        = "=VAL*1000"
    legend_formula = "=round(VAL,1)" 
    color          = element(var.circonus_color_palette, 0)
  }

  metric {
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "external - average_latency - ${lookup(var.urls[count.index],"url")}|ST[service:${lookup(var.urls[count.index],"service")},host:all,ip:all]"
    name           = "Average External Call Latency (ms)"
    axis           = "left"
    alpha          = 0.8
    stack          = 0
    formula        = "=VAL*1000"
    legend_formula = "=round(VAL,1)" 
    color          = element(var.circonus_color_palette, 1)
  }

  metric {
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "transaction - average_latency - ${lookup(var.urls[count.index],"url")}|ST[service:${lookup(var.urls[count.index],"service")},host:all,ip:all,method:GET]"
    name           = "Average Transaction Latency - GET (ms)"
    axis           = "left"
    alpha          = 0.8
    stack          = 0
    formula        = "=VAL*1000"
    legend_formula = "=round(VAL,1)" 
    color          = element(var.circonus_color_palette, 2)
  }

  metric {
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "transaction - average_latency - ${lookup(var.urls[count.index],"url")}|ST[service:${lookup(var.urls[count.index],"service")},host:all,ip:all,method:POST]"
    name           = "Average Transaction Latency - POST (ms)"
    axis           = "left"
    alpha          = 0.8
    stack          = 0
    formula        = "=VAL*1000"
    legend_formula = "=round(VAL,1)" 
    color          = element(var.circonus_color_palette, 3)
  }
  tags = [ "service:${lower(lookup(var.urls[count.index],"service"))}" ]
}

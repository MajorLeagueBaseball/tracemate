resource "circonus_graph" "url_response_time_heatmap_graph" {
  count       = length(var.urls)
  name        = "${lookup(var.urls[count.index],"url")} - Heatmap Response Time"
  description = "Heatmap of response time for this URL across all hosts"
  notes       = ""
  graph_style = "area"
  line_style  = "interpolated"

  metric {
    metric_type    = "histogram"
    check          = var.apm_check_cid
    metric_name    = "db - latency - ${lookup(var.urls[count.index],"url")}|ST[service:${lookup(var.urls[count.index],"service")},host:all,ip:all]"
    name           = "Heatmap DB Latency (ms)"
    axis           = "left"
    alpha          = 0.8
    stack          = 0
    formula        = "=VAL*1000"
    legend_formula = "=round(VAL,1)" 
    color          = element(var.circonus_color_palette, 0)
  }

  metric {
    metric_type    = "histogram"
    check          = var.apm_check_cid
    metric_name    = "external - latency - ${lookup(var.urls[count.index],"url")}|ST[service:${lookup(var.urls[count.index],"service")},host:all,ip:all]"
    name           = "Heatmap External Call Latency (ms)"
    axis           = "left"
    alpha          = 0.8
    stack          = 0
    formula        = "=VAL*1000"
    legend_formula = "=round(VAL,1)" 
    color          = element(var.circonus_color_palette, 1)
  }

  metric {
    metric_type    = "histogram"
    check          = var.apm_check_cid
    metric_name    = "transaction - latency - ${lookup(var.urls[count.index],"url")}|ST[service:${lookup(var.urls[count.index],"service")},host:all,ip:all,method:GET]"
    name           = "Heatmap Transaction Latency - GET (ms)"
    axis           = "left"
    alpha          = 0.8
    stack          = 0
    formula        = "=VAL*1000"
    legend_formula = "=round(VAL,1)" 
    color          = element(var.circonus_color_palette, 2)
  }

  metric {
    metric_type    = "histogram"
    check          = var.apm_check_cid
    metric_name    = "transaction - latency - ${lookup(var.urls[count.index],"url")}|ST[service:${lookup(var.urls[count.index],"service")},host:all,ip:all,method:POST]"
    name           = "Heatmap Transaction Latency - POST (ms)"
    axis           = "left"
    alpha          = 0.8
    stack          = 0
    formula        = "=VAL*1000"
    legend_formula = "=round(VAL,1)" 
    color          = element(var.circonus_color_palette, 3)
  }
  tags = [ "service:${lower(lookup(var.urls[count.index],"service"))}" ]
}

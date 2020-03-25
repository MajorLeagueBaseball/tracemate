resource "circonus_graph" "url_throughput_graph" {
  count       = length(var.urls)
  name        = "${lookup(var.urls[count.index],"url")} - Throughput"
  description = "Total requests per time window for a url across all hosts"
  notes       = ""
  graph_style = "line"
  line_style  = "interpolated"

  metric {
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "transaction - request_count - ${lookup(var.urls[count.index],"url")}|ST[service:${lookup(var.urls[count.index],"service")},host:all,ip:all,method:GET]"
    name        = "Throughput - GET"
    axis        = "left"
    color       = element(var.circonus_color_palette, 0)
  }

  metric {
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "transaction - request_count - ${lookup(var.urls[count.index],"url")}|ST[service:${lookup(var.urls[count.index],"service")},host:all,ip:all,method:POST]"
    name        = "Throughput - POST"
    axis        = "left"
    color       = element(var.circonus_color_palette, 1)
  }

  tags = [ "service:${lower(lookup(var.urls[count.index],"service"))}" ]
}

resource "circonus_graph" "url_error_rate_graph" {
  count       = length(var.urls)
  name        = "${lookup(var.urls[count.index],"url")} - Error Rate"
  description = "Total errors (http status_code >= 400) per time window for a URL"
  notes       = ""
  graph_style = "line"
  line_style  = "interpolated"

  metric {
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "transaction - server_error_count - ${lookup(var.urls[count.index],"url")}|ST[service:${lookup(var.urls[count.index],"service")},host:all,ip:all,method:GET]"
    name           = "Server Error Rate - GET"
    axis           = "left"
    color          = element(var.circonus_color_palette, 0)
  }

  metric {
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "transaction - server_error_count - ${lookup(var.urls[count.index],"url")}|ST[service:${lookup(var.urls[count.index],"service")},host:all,ip:all,method:POST]"
    name           = "Server Error Rate - POST"
    axis           = "left"
    color          = element(var.circonus_color_palette, 1)
  }

  metric {
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "transaction - client_error_count - ${lookup(var.urls[count.index],"url")}|ST[service:${lookup(var.urls[count.index],"service")},host:all,ip:all,method:GET]"
    name           = "Client Error Rate - GET"
    axis           = "left"
    color          = element(var.circonus_color_palette, 2)
  }

  metric {
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "transaction - client_error_count - ${lookup(var.urls[count.index],"url")}|ST[service:${lookup(var.urls[count.index],"service")},host:all,ip:all,method:POST]"
    name           = "Client Error Rate - POST"
    axis           = "left"
    color          = element(var.circonus_color_palette, 2)
  }

  tags = [ "service:${lower(lookup(var.urls[count.index],"service"))}" ]
}

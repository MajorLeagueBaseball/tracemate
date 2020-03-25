resource "circonus_graph" "service_response_time_histogram_graph" {
  for_each    = var.top-level-services
  name        = "${each.key} - Heatmap of Response Time"
  description = "Heatmap of response time overall for this service"
  notes       = "Latencies are in milliseconds"
  graph_style = "area"
  line_style  = "interpolated"

  metric {
    active          = replace(replace(each.value.type, "message", "0"), "transactional", "1")
    metric_type    = "histogram"
    check          = var.apm_check_cid
    metric_name    = "transaction - latency - all|ST[service:${each.key},host:all,ip:all,method:GET]"
    name           = "Heatmap Transaction Latency - GET (ms)"
    axis           = "left"
    alpha          = 1.0
    stack          = 0
    formula        = "=VAL*1000"
    legend_formula = "=round(VAL,1)"
    color          = element(var.circonus_color_palette, 0)
  }

  metric {
    active          = replace(replace(each.value.type, "message", "1"), "transactional", "0")
    metric_type    = "histogram"
    check          = var.apm_check_cid
    metric_name    = "transaction - latency - all|ST[service:${each.key},host:all,ip:all]"
    name           = "Heatmap Transaction Latency (ms)"
    axis           = "left"
    alpha          = 1.0
    stack          = 0
    formula        = "=VAL*1000"
    legend_formula = "=round(VAL,1)"
    color          = element(var.circonus_color_palette, 0)
  }


  metric {
    active          = replace(replace(each.value.type, "message", "0"), "transactional", "1")
    metric_type    = "histogram"
    check          = var.apm_check_cid
    metric_name    = "transaction - latency - all|ST[service:${each.key},host:all,ip:all,method:POST]"
    name           = "Heatmap Transaction Latency - POST (ms)"
    axis           = "left"
    alpha          = 1.0
    stack          = 0
    formula        = "=VAL*1000"
    legend_formula = "=round(VAL,1)"
    color          = element(var.circonus_color_palette, 1)
  }
  tags = [ "service:${lower(each.key)}" ]
}

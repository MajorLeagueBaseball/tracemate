resource "circonus_graph" "service_response_time_graph" {
  for_each    = var.top-level-services
  name        = "${each.key} - Average Response Time"
  description = "Average response time overall for this service"
  notes       = "Latencies are in milliseconds"
  graph_style = "area"
  line_style  = "stepped"

  metric {
    active          = replace(replace(each.value.type, "message", "0"), "transactional", "1")
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "db - average_latency - all|ST[service:${each.key},host:all,ip:all]"
    name           = "Average DB Latency (ms)"
    axis           = "left"
    alpha          = 0.8
    formula        = "=VAL*1000"
    legend_formula = "=round(VAL,1)"
    color          = element(var.circonus_color_palette, 0)
  }

  metric {
    active          = replace(replace(each.value.type, "message", "0"), "transactional", "1")
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "external - average_latency - all|ST[service:${each.key},host:all,ip:all]"
    name           = "Average External Call Latency (ms)"
    axis           = "left"
    alpha          = 0.8
    formula        = "=VAL*1000"
    legend_formula = "=round(VAL,1)"
    color          = element(var.circonus_color_palette, 1)
  }

  metric {
    active          = replace(replace(each.value.type, "message", "0"), "transactional", "1")
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "transaction - average_latency - all|ST[service:${each.key},host:all,ip:all,method:GET]"
    name           = "Average Transaction Latency - GET (ms)"
    axis           = "left"
    alpha          = 0.8
    formula        = "=VAL*1000"
    legend_formula = "=round(VAL,1)"
    color          = element(var.circonus_color_palette, 2)
  }

  metric {
    active          = replace(replace(each.value.type, "message", "1"), "transactional", "0")
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "transaction - average_latency - all|ST[service:${each.key},host:all,ip:all]"
    name           = "Average Transaction Latency (ms)"
    axis           = "left"
    alpha          = 0.8
    formula        = "=VAL*1000"
    legend_formula = "=round(VAL,1)"
    color          = element(var.circonus_color_palette, 2)
  }

  metric {
    active          = replace(replace(each.value.type, "message", "0"), "transactional", "1")
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "transaction - average_latency - all|ST[service:${each.key},host:all,ip:all,method:POST]"
    name           = "Average Transaction Latency - POST (ms)"
    axis           = "left"
    alpha          = 0.8
    formula        = "=VAL*1000"
    legend_formula = "=round(VAL,1)"
    color          = element(var.circonus_color_palette, 3)
  }
  tags = [ "service:${lower(each.key)}" ]
}

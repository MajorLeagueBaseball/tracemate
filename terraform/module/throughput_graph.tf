resource "circonus_graph" "service_throughput_graph" {
  for_each    = var.top-level-services
  name        = "${each.key} - Throughput"
  description = "Total requests per time window for a service"
  notes       = ""
  graph_style = "line"
  line_style  = "interpolated"

  metric {
    active          = replace(replace(each.value.type, "message", "0"), "transactional", "1")
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "transaction - request_count - all|ST[service:${each.key},host:all,ip:all,method:GET]"
    name           = "Throughput - GET"
    axis           = "left"
    color          = element(var.circonus_color_palette, 0)
  }

  metric {
    active          = replace(replace(each.value.type, "message", "1"), "transactional", "0")
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "transaction - request_count - all|ST[service:${each.key},host:all,ip:all]"
    name           = "Throughput"
    axis           = "left"
    color          = element(var.circonus_color_palette, 0)
  }

  metric {
    active          = replace(replace(each.value.type, "message", "0"), "transactional", "1")
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "transaction - request_count - all|ST[service:${each.key},host:all,ip:all,method:POST]"
    name           = "Throughput - POST"
    axis           = "left"
    color          = element(var.circonus_color_palette, 1)
  }

  tags = [ "service:${lower(each.key)}" ]
}

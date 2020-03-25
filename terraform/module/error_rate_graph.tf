resource "circonus_graph" "service_error_rate_graph" {
  for_each    = var.top-level-services
  name        = "${each.key} - Error rate"
  description = "Total errors (http status_code >= 400) per time window for a service"
  notes       = ""
  graph_style = "line"
  line_style  = "interpolated"

  metric {
    active          = replace(replace(lookup(var.top-level-services[each.key], "type"), "message", "0"), "transactional", "1")
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "transaction - server_error_count - all|ST[service:${each.key},host:all,ip:all,method:GET]"
    name           = "Server Error Rate - GET"
    axis           = "left"
    color          = element(var.circonus_color_palette, 0)
  }

  metric {
    active          = replace(replace(lookup(var.top-level-services[each.key], "type"), "message", "0"), "transactional", "1")
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "transaction - server_error_count - all|ST[service:${each.key},host:all,ip:all,method:POST]"
    name           = "Server Error Rate - POST"
    axis           = "left"
    color          = element(var.circonus_color_palette, 1)
  }

  metric {
    active          = replace(replace(lookup(var.top-level-services[each.key], "type"), "message", "0"), "transactional", "1")
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "transaction - client_error_count - all|ST[service:${each.key},host:all,ip:all,method:GET]"
    name           = "Client Error Rate - GET"
    axis           = "left"
    color          = element(var.circonus_color_palette, 2)
  }

  metric {
    active          = replace(replace(lookup(var.top-level-services[each.key], "type"), "message", "0"), "transactional", "1")
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "transaction - client_error_count - all|ST[service:${each.key},host:all,ip:all,method:POST]"
    name           = "Client Error Rate - POST"
    axis           = "left"
    color          = element(var.circonus_color_palette, 2)
  }

  metric {
    active          = replace(replace(lookup(var.top-level-services[each.key], "type"), "message", "1"), "transactional", "0")
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "transaction - apm_error_count - all|ST[service:${each.key},host:all,ip:all]"
    name           = "APM Error count"
    axis           = "left"
    color          = element(var.circonus_color_palette, 3)
  }

  tags = [ "service:${lower(each.key)}" ]
}

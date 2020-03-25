resource "circonus_graph" "cpu" {
  for_each    = var.top-level-services
  name        = "${each.key} - CPU"
  description = "CPU utilization"
  notes       = ""
  graph_style = "line"
  line_style  = "interpolated"

  metric {
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "system - cpu - total - pct|ST[service:${each.key},host:all,ip:all]"
    name           = "Average - System CPU %"
    axis           = "left"
    formula        = "=VAL*100"
    color          = element(var.circonus_color_palette, 2)
  }

  metric {
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "system - process - cpu - total - pct|ST[service:${each.key},host:all,ip:all]"
    name           = "Average - Process CPU %"
    axis           = "left"
    formula        = "=VAL*100"
    color          = element(var.circonus_color_palette, 2)
  }
  tags = [ "service:${lower(each.key)}" ]
}

resource "circonus_graph" "service_errors_slo_graph" {
  for_each    = local.sre_errors_services
  name        = "${each.key} - Successful Response SLO Error Budget"
  description = "Successful response SLO and error budget spend"
  graph_style = "area"
  line_style  = "interpolated"
  left = {
    min = 0
  }

  metric {
    metric_type    = "caql"
    caql           = <<-EOT
find:average("transaction - server_error_count - all", "and(host:all,method:GET,service:${each.key})")
| label("SLO Violating request count")
EOT
    name           = "SLO Violating request count"
    axis           = "right"
    alpha          = 0.5
    legend_formula = "=round(VAL,0)"
    color          = "#d74b2c"
  }

  metric {
    metric_type    = "caql"
    caql           = <<-EOT
${(100 - each.value["window_percentile"])/100}
| label("SLO Spend Target (${100 - each.value["window_percentile"]}%)")
EOT
    name           = "SLO Spend Target"
    axis           = "left"
    alpha          = 0.01
    legend_formula = "=round(VAL,0)"
    color          = "#080808"
  }


  metric {
    metric_type    = "caql"
    caql           = <<-EOT
integrate:while(prefill=4w){
   time:tz("${each.value["budget_timezone"]}", "month"),
   find:average("transaction - server_error_count - all", "and(host:all,method:GET,service:${each.key})")
   | fill(0)
}
/
(
   find:average("transaction - request_count - all", "and(host:all,method:GET,service:${each.key})")
   | fill(1)
   | rolling:sum(4w, skip=1h)
   | op:prod(60) | op:prod(VIEW_PERIOD)
)
| label("Response Budget spent per month")
EOT
    name           = "Response Budget spent per month"
    axis           = "left"
    alpha          = 0.8
    legend_formula = "=round(VAL,4)"
    color          = "#1177dc"
  }


  tags = [ "service:${lower(each.key)}" ]
}

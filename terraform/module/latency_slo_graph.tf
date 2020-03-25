resource "circonus_graph" "service_latency_slo_graph" {
  for_each    = local.sre_latency_services
  name        = "${each.key} - Latency SLO Error Budget"
  description = "Latency SLO and error budget spend"
  notes       = "Latencies are in milliseconds"
  graph_style = "area"
  line_style  = "interpolated"

  metric {
    metric_type    = "caql"
    caql           = <<-EOT
find:histogram("transaction - latency - all", "and(host:all,method:GET,service:${each.key})")
| histogram:merge()
| histogram:count_above(${each.value["window_threshold_ms"] / 1000})
| label("SLO Violating request count (${each.value["window_threshold_ms"]} ms)")
EOT
    name           = "SLO Violating request count (${each.value["window_threshold_ms"]} ms)"
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
   time:tz("America/New_York", "month"),
   find:histogram("transaction - latency - all", "and(host:all,method:GET,service:${each.key})")
   | histogram:merge()
   | histogram:count_above(${each.value["window_threshold_ms"] / 1000})
}
/
(
   find:histogram("transaction - latency - all", "and(host:all,method:GET,service:${each.key})")
   | histogram:merge()
   | histogram:count()
   | rolling:sum(4w, skip=1h)
   | op:prod(60) | op:prod(VIEW_PERIOD)
)
| label("Latency Budget spent per month")
EOT
    name           = "Latency Budget spent per month"
    axis           = "left"
    alpha          = 0.8
    legend_formula = "=round(VAL,4)"
    color          = "#1177dc"
  }
  tags = [ "service:${lower(each.key)}" ]
}

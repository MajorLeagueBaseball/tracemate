resource "circonus_graph" "service_top_five_graph" {
  for_each    = var.top-level-services
  name        = "${each.key} - Top 5 Endpoints"
  description = "Top 5 Endpoints in average response time overall for this service"
  notes       = ""
  graph_style = "line"
  line_style  = "interpolated"

  metric {
    metric_type = "caql"
    caql        = "find:average(\"transaction - average_latency - *\", \"and(service:${each.key},host:all,__check_uuid:${var.apm_check_uuid},not(__name:b'dHJhbnNhY3Rpb24gLSBhdmVyYWdlX2xhdGVuY3kgLSBhbGwK'))\") | top(5) | label(\"%n\")"
    name        = "Top 5 in latency"
    axis        = "left"
    //formula        = "=VAL*1000"
    legend_formula = "=round(VAL * 1000,1)"
    color       = element(var.circonus_color_palette, 0)
  }  
  tags = [ "service:${lower(each.key)}" ]
}

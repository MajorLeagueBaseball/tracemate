resource "circonus_graph" "nodejs_heap" {
  for_each    = {for k,v in var.top-level-services: k => v if v.runtime == "nodejs"}
  name        = "${each.key} - node.js Memory Stats"
  description = "node.js max and used heap averages across all instances"
  notes       = ""
  graph_style = "area"
  line_style  = "interpolated"

  metric {
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "nodejs - memory - heap - allocated|ST[service:${each.key},host:all,ip:all]"
    name        = "Average - Max Heap"
    axis        = "left"
    alpha       = 0.8
    color       = element(var.circonus_color_palette, 2)
  }

  metric {
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "nodejs - memory - heap - used|ST[service:${each.key},host:all,ip:all]"
    name        = "Average - Used Heap"
    axis        = "left"
    alpha       = 0.8
    color       = element(var.circonus_color_palette, 3)
  }  

  tags = [ "service:${lower(each.key)}" ]
}

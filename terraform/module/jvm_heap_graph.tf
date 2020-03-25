resource "circonus_graph" "jvm_heap" {
  for_each    = {for k,v in var.top-level-services: k => v if v.runtime == "java"}
  name        = "${each.key} - JVM Memory Stats"
  description = "JVM Max and used heap averages across all instances"
  notes       = ""
  graph_style = "area"
  line_style  = "interpolated"

  metric {
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "jvm - memory - heap - max|ST[service:${each.key},host:all,ip:all]"
    name        = "Average - Max Heap"
    axis        = "left"
    alpha       = 0.8
    color       = element(var.circonus_color_palette, 2)
  }

  metric {
    metric_type    = "numeric"
    check          = var.apm_check_cid
    metric_name    = "jvm - memory - heap - used|ST[service:${each.key},host:all,ip:all]"
    name        = "Average - Used Heap"
    axis        = "left"
    alpha       = 0.8
    color       = element(var.circonus_color_palette, 3)
  }  

  tags = [ "service:${lower(each.key)}" ]
}

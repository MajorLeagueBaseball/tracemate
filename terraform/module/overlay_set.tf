// resource "circonus_overlay_set" "service_response_time_comparison" {
//   count       = "${length(var.top-level-services)}"
//   title       = "Comparisons"
//   graph_cid   = "${element(circonus_graph.service_response_time_graph.*.id,count.index)}"

//   overlays {
//     id            = "yesterday"
//     data_opts {
//       graph_title = "Current Graph"
//       graph_uuid  = "${element(split("/", element(circonus_graph.service_response_time_graph.*.id, count.index)),2)}"
//       x_shift     = "1d"
//     }
//     ui_specs {
//       id          = "yesterday"
//       z           = "-1"
//       label       = "Current Graph (Time-Shift: 1d)"
//       type        = "graph_comparison"
//       decouple    = false
//     }
//   }

//   overlays {
//     id            = "last_week"
//     data_opts {
//       graph_title = "Current Graph"
//       graph_uuid  = "${element(split("/", element(circonus_graph.service_response_time_graph.*.id, count.index)),2)}"
//       x_shift     = "7d"
//     }
//     ui_specs {
//       id          = "last_week"
//       z           = "-2"
//       label       = "Current Graph (Time-Shift: 7d)"
//       type        = "graph_comparison"
//       decouple    = false
//     }
//   }
// }

resource "circonus_dashboard" "service_dash" {
  for_each     = var.top-level-services
  title        = "${each.key} - Performance"
  account_default = false
  shared = true

  grid_layout = {
    width = 8
    height = 7
  }

  options {
    hide_grid = true
    text_size = 14
  }

  widget {
    active = true
    height = 1
    width = 8
    origin = "a6"
    type = "html"
    name = "HTML"
    widget_id = "w0"
    settings {
      markup = <<EOF
      <p>All latency times are in milliseconds</p>
      <ul>
        <li>
          <a target="_blank" href="http://${var.team_name}.${var.jaeger_base_url}/search?limit=200&lookback=2h&service=${each.key}">Jaeger search the trailing 2 hours (limit 200)</a>
        </li>
        <li>
          <a target="_blank" href="http://${var.team_name}.${var.jaeger_base_url}/search?limit=200&lookback=2d&service=${each.key}">Jaeger search the trailing 2 days (limit 200)</a>
        </li>
        <li>
          <a target="_blank" href="http://${var.team_name}.${var.jaeger_base_url}/search?limit=200&lookback=2w&service=${each.key}">Jaeger search the trailing 2 weeks (limit 200)</a>
        </li>
      </ul>
EOF
    }
  }

  widget {
    active = true
    height = 2
    width = 6
    origin = "a0"
    type = "graph"
    widget_id = "w1"
    name = "Graph"

    settings {
      date_window = "global"
      graph_uuid = element(split("/", circonus_graph.service_response_time_graph[each.key].id),2)
      show_flags = true
    }
  }

  widget {
    active = true
    height = 2
    width = 2
    origin = "g0"
    type = "graph"
    widget_id = "w2"
    name = "Graph"

    settings {
      date_window = "global"
      graph_uuid = element(split("/", circonus_graph.service_throughput_graph[each.key].id),2)
      show_flags = true
    }
  }


  widget {
    active = true
    height = 2
    width = each.value.sre.latency != null ? 3 : 6
    origin = "a2"
    type = "graph"
    widget_id = "w7"
    name = "Graph"

    settings {
      date_window = "global"
      graph_uuid = element(split("/", circonus_graph.service_response_time_histogram_graph[each.key].id),2)
      show_flags = true
    }
  }

  dynamic "widget" {
    for_each = each.value.sre.latency == null ? [] : [circonus_graph.service_latency_slo_graph[each.key].id]
    content {
      active = true
      height = 2
      width = 3
      origin = "d2"
      type = "graph"
      widget_id = "w9"
      name = "Graph"

      settings {
        date_window = "global"
        graph_uuid = element(split("/", widget.value),2)
        show_flags = true
      }
    }
  }

  widget {
    active = true
    height = 2
    width = 3
    origin = "a4"
    type = "graph"
    widget_id = "w4"
    name = "Graph"

    settings {
      date_window = "global"
      graph_uuid = element(split("/", circonus_graph.service_top_five_graph[each.key].id),2)
      show_flags = true
    }
  }

  widget {
    active = true
    height = each.value.sre.errors != null ? 1 : 2
    width = 3
    origin = "d4"
    type = "graph"
    widget_id = "w5"
    name = "Graph"

    settings {
      date_window = "global"
      graph_uuid = element(split("/", circonus_graph.service_error_rate_graph[each.key].id),2)
      show_flags = true
    }
  }

  dynamic "widget" {
    for_each = each.value.sre.errors == null ? [] : [circonus_graph.service_errors_slo_graph[each.key].id]
    content {
      active = true
      height = 1
      width = 3
      origin = "d5"
      type = "graph"
      widget_id = "w10"
      name = "Graph"

      settings {
        date_window = "global"
        graph_uuid = element(split("/", widget.value),2)
        show_flags = true
      }
    }
  }


  widget {
    active = true
    height = 1
    width = 2
    origin = "g2"
    type = "graph"
    widget_id = "w6"
    name = "Graph"

    settings {
      date_window = "global"
      graph_uuid = element(split("/", circonus_graph.cpu[each.key].id),2)
      show_flags = true
    }
  }

  dynamic "widget" {
    for_each = each.value.runtime == "java" ? [circonus_graph.jvm_heap[each.key].id] : []
    content {
      active = true
      height = 2
      width = 2
      origin = "g3"
      type = "graph"
      widget_id = "w8"
      name = "Graph"

      settings {
        date_window = "global"
        graph_uuid = element(split("/", widget.value),2)
        show_flags = true
      }
    }
  }

  dynamic "widget" {
    for_each = each.value.runtime == "nodejs" ? [circonus_graph.nodejs_heap[each.key].id] : []
    content {
      active = true
      height = 2
      width = 2
      origin = "g3"
      type = "graph"
      widget_id = "w8"
      name = "Graph"

      settings {
        date_window = "global"
        graph_uuid = element(split("/", widget.value),2)
        show_flags = true
      }
    }
  }
}

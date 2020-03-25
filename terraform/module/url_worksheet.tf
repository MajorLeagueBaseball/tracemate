resource "circonus_worksheet" "url-worksheet" {
  count        = length(var.urls)
  title        = "${lookup(var.urls[count.index], "service")} - ${lookup(var.urls[count.index], "url")}"
  graphs       = [
      element(circonus_graph.url_throughput_graph.*.id, count.index),
      element(circonus_graph.url_response_time_graph.*.id, count.index),
      element(circonus_graph.url_response_time_heatmap_graph.*.id, count.index),
      element(circonus_graph.url_error_rate_graph.*.id, count.index),
  ]
}


output "service_dashboards" {
  value = circonus_dashboard.service_dash
}

output "cpu_graphs" {
  value = circonus_graph.cpu
}

output "average_response_time_graphs" {
  value = circonus_graph.service_response_time_graph
}

output "top-level-service-names" {
  value = keys(var.top-level-services)
}

output "circonus_api_key" {
  value = var.circonus_api_key
}

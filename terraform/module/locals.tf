locals {
  sre_latency_services = {
    for s, val in var.top-level-services: s => val.sre.latency if val.sre.latency != null
  }
  sre_errors_services = {
    for s, val in var.top-level-services: s => val.sre.errors if val.sre.errors != null
  }
  sre_contacts = {
    for s, val in var.top-level-services: s => coalesce(val.sre.override, var.sre)
  }
}

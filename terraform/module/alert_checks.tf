// ---------------------- CAQL checks to calculate latency burn rate

resource "circonus_check" "caql_five_minute_latency_rate" {
  for_each = {for k,v in local.sre_latency_services: k => v if v.severity != null}
  name = "${each.key} - 5min Latency Error Budget Burn Rate"
  target = "q._caql"

  collector {
    id = "/broker/1490"
  }

  caql {
    query = "find:histogram(\"transaction - latency - all\", \"and(host:all,method:GET,service:${each.key})\")|window:merge(5M,period=1M)|histogram:inverse_percentile(${each.value.window_threshold_ms / 1000})|op:neg()|op:sum(100)|label(\"burn_rate_5min\")"
  }

  metric {
    name = "burn_rate_5min"
    type = "numeric"
    active = true
    unit = "%"
  }

  tags = [
    "service:${lower(each.key)}"
  ]
}

resource "circonus_check" "caql_thirty_minute_latency_rate" {
  for_each = {for k,v in local.sre_latency_services: k => v if v.severity != null}
  name = "${each.key} - 30min Latency Error Budget Burn Rate"
  target = "q._caql"

  collector {
    id = "/broker/1490"
  }

  caql {
    query = "find:histogram(\"transaction - latency - all\", \"and(host:all,method:GET,service:${each.key})\")|window:merge(30M,period=5M)|histogram:inverse_percentile(${each.value.window_threshold_ms / 1000})|op:neg()|op:sum(100)|label(\"burn_rate_30min\")"
  }

  metric {
    name = "burn_rate_30min"
    type = "numeric"
    active = true
    unit = "%"
  }

  tags = [
    "service:${lower(each.key)}"
  ]
}

resource "circonus_check" "caql_one_hour_latency_rate" {
  for_each = {for k,v in local.sre_latency_services: k => v if v.severity != null}
  name = "${each.key} - 1hour Latency Error Budget Burn Rate"
  target = "q._caql"

  collector {
    id = "/broker/1490"
  }

  caql {
    query = "find:histogram(\"transaction - latency - all\", \"and(host:all,method:GET,service:${each.key})\")|window:merge(1h,period=10M)|histogram:inverse_percentile(${each.value.window_threshold_ms / 1000})|op:neg()|op:sum(100)|label(\"burn_rate_1hour\")"
  }

  metric {
    name = "burn_rate_1hour"
    type = "numeric"
    active = true
    unit = "%"
  }

  tags = [
    "service:${lower(each.key)}"
  ]
}

resource "circonus_check" "caql_six_hour_latency_rate" {
  for_each = {for k,v in local.sre_latency_services: k => v if v.severity != null}
  name = "${each.key} - 6hour Latency Error Budget Burn Rate"
  target = "q._caql"

  collector {
    id = "/broker/1490"
  }

  caql {
    query = "find:histogram(\"transaction - latency - all\", \"and(host:all,method:GET,service:${each.key})\")|window:merge(6h,period=1h)|histogram:inverse_percentile(${each.value.window_threshold_ms / 1000})|op:neg()|op:sum(100)|label(\"burn_rate_6hour\")"
  }

  metric {
    name = "burn_rate_6hour"
    type = "numeric"
    active = true
    unit = "%"
  }

  tags = [
    "service:${lower(each.key)}"
  ]
}

// ---------------------- rule sets to alert on latency burn rate

resource "circonus_rule_set" "latency_five_minute_burn" {
  for_each = {for k,v in local.sre_latency_services: k => v if v.severity != null}
  check = circonus_check.caql_five_minute_latency_rate[each.key].checks[0]
  metric_name = "burn_rate_5min"
  notes = "This service is burning through its latency error budget very quickly"
  link = "https://${each.value.url_base}.circonus.com/trending/graphs/view/${element(split("/", circonus_graph.service_latency_slo_graph[each.key].id),2)}"

  if {
    then {
      severity = each.value.severity
      after    = "360"
    }
    value {
      over {
        last = "300"
        using = "average"
      }
      max_value = "20"
    }
  }
  tags = [
    "service:${lower(each.key)}"
  ]
}

resource "circonus_rule_set" "latency_thirty_minute_burn" {
  for_each = {for k,v in local.sre_latency_services: k => v if v.severity != null}
  check = circonus_check.caql_thirty_minute_latency_rate[each.key].checks[0]
  metric_name = "burn_rate_30min"
  notes = "This service is burning through its latency error budget moderately quickly"
  link = "https://${each.value.url_base}.circonus.com/trending/graphs/view/${element(split("/", circonus_graph.service_latency_slo_graph[each.key].id),2)}"

  if {
    then {
      severity = each.value.severity
      after    = "360"
    }
    value {
      over {
        last = "60"
        using = "average"
      }
      max_value = "20"
    }
  }
  tags = [
    "service:${lower(each.key)}"
  ]

}

resource "circonus_rule_set" "latency_one_hour_burn" {
  for_each = {for k,v in local.sre_latency_services: k => v if v.severity != null}
  check = circonus_check.caql_one_hour_latency_rate[each.key].checks[0]
  metric_name = "burn_rate_1hour"
  notes = "This service is burning through its latency error budget moderately"
  link = "https://${each.value.url_base}.circonus.com/trending/graphs/view/${element(split("/", circonus_graph.service_latency_slo_graph[each.key].id),2)}"

  if {
    then {
      severity = each.value.severity
      after    = "360"
    }
    value {
      over {
        last = "60"
        using = "average"
      }
      max_value = "10"
    }
  }
  tags = [
    "service:${lower(each.key)}"
  ]

}

resource "circonus_rule_set" "latency_six_hour_burn" {
  for_each = {for k,v in local.sre_latency_services: k => v if v.severity != null}
  check = circonus_check.caql_six_hour_latency_rate[each.key].checks[0]
  metric_name = "burn_rate_6hour"
  notes = "This service is burning through its latency error budget slowly"
  link = "https://${each.value.url_base}.circonus.com/trending/graphs/view/${element(split("/", circonus_graph.service_latency_slo_graph[each.key].id),2)}"

  if {
    then {
      severity = each.value.severity
      after    = "360"
    }
    value {
      over {
        last = "60"
        using = "average"
      }
      max_value = "10"
    }
  }

  tags = [
    "service:${lower(each.key)}"
  ]
}

// ---------------------- rule set groups to notify when 2 rulesets above fail at the same time

resource "circonus_rule_set_group" "latency_burn_too_fast_short" {
  for_each = {for k,v in local.sre_latency_services: k => v if v.severity != null}
  name = "${each.key} - Slow responses have spiked (latency is bad)"

  notify {
    sev1 = local.sre_contacts[each.key].sev1
    sev2 = local.sre_contacts[each.key].sev2
    sev3 = local.sre_contacts[each.key].sev3
    sev4 = local.sre_contacts[each.key].sev4
    sev5 = local.sre_contacts[each.key].sev5
  }

  formula {
    expression = "A and B"
    raise_severity = each.value.severity
    wait = 1
  }

  condition {
    index = 1
    rule_set = circonus_rule_set.latency_five_minute_burn[each.key].rule_set_id
    matching_severities = ["${each.value.severity}"]
  }
  condition {
    index = 2
    rule_set = circonus_rule_set.latency_thirty_minute_burn[each.key].rule_set_id
    matching_severities = ["${each.value.severity}"]
  }
}

resource "circonus_rule_set_group" "latency_burn_too_fast_long" {
  for_each = {for k,v in local.sre_latency_services: k => v if v.severity != null}
  name = "${each.key} - Slow responses are increasing (latency is increasing)"

  notify {
    sev1 = local.sre_contacts[each.key].sev1
    sev2 = local.sre_contacts[each.key].sev2
    sev3 = local.sre_contacts[each.key].sev3
    sev4 = local.sre_contacts[each.key].sev4
    sev5 = local.sre_contacts[each.key].sev5
  }

  formula {
    expression = "A and B"
    raise_severity = each.value.severity
    wait = 1
  }

  condition {
    index = 1
    rule_set = circonus_rule_set.latency_one_hour_burn[each.key].rule_set_id
    matching_severities = ["${each.value.severity}"]
  }
  condition {
    index = 2
    rule_set = circonus_rule_set.latency_six_hour_burn[each.key].rule_set_id
    matching_severities = ["${each.value.severity}"]
  }
}

// ---------------------- CAQL checks to calculate response error burn rate

# resource "circonus_check" "caql_five_minute_error_rate" {
#   for_each = {for k,v in local.sre_errors_services: k => v if v.severity != null}
#   name = "${each.key} - 5min Response Error Budget Burn Rate"
#   target = "q._caql"

#   collector {
#     id = "/broker/1490"
#   }

#   caql {
#     query = "op:div{find:average(\"transaction - server_error_count - all\",\"and(host:all,method:GET,service:${each.key})\")|fill(0)|stats:sum()|window:sum(5M,period=1M),find:average(\"transaction - request_count - all\",\"and(host:all,method:GET,service:${each.key})\")|fill(1)|stats:sum()|window:sum(5M,period=1M)}|op:prod(100)|label(\"burn_rate_5min\")"
#   }

#   metric {
#     name = "burn_rate_5min"
#     type = "numeric"
#     active = true
#     unit = "%"
#   }

#   tags = [
#     "service:${lower(each.key)}"
#   ]
# }

# resource "circonus_check" "caql_thirty_minute_error_rate" {
#   for_each = {for k,v in local.sre_errors_services: k => v if v.severity != null}
#   name = "${each.key} - 30min Response Error Budget Burn Rate"
#   target = "q._caql"

#   collector {
#     id = "/broker/1490"
#   }

#   caql {
#     query = "op:div{find:average(\"transaction - server_error_count - all\",\"and(host:all,method:GET,service:${each.key})\")|fill(0)|stats:sum()|window:sum(30M,period=5M),find:average(\"transaction - request_count - all\",\"and(host:all,method:GET,service:${each.key})\")|fill(1)|stats:sum()|window:sum(30M,period=5M)}|op:prod(100)|label(\"burn_rate_30min\")"
#   }

#   metric {
#     name = "burn_rate_30min"
#     type = "numeric"
#     active = true
#     unit = "%"
#   }

#   tags = [
#     "service:${lower(each.key)}"
#   ]
# }

# resource "circonus_check" "caql_one_hour_error_rate" {
#   for_each = {for k,v in local.sre_errors_services: k => v if v.severity != null}
#   name = "${each.key} - 1hour Response Error Budget Burn Rate"
#   target = "q._caql"

#   collector {
#     id = "/broker/1490"
#   }

#   caql {
#     query = "op:div{find:average(\"transaction - server_error_count - all\",\"and(host:all,method:GET,service:${each.key})\")|fill(0)|stats:sum()|window:sum(1h,period=10M),find:average(\"transaction - request_count - all\",\"and(host:all,method:GET,service:${each.key})\")|fill(1)|stats:sum()|window:sum(1h,period=10M)}|op:prod(100)|label(\"burn_rate_1hour\")"
#   }

#   metric {
#     name = "burn_rate_1hour"
#     type = "numeric"
#     active = true
#     unit = "%"
#   }

#   tags = [
#     "service:${lower(each.key)}"
#   ]
# }

# resource "circonus_check" "caql_six_hour_error_rate" {
#   for_each = {for k,v in local.sre_errors_services: k => v if v.severity != null}
#   name = "${each.key} - 6hour Response Error Budget Burn Rate"
#   target = "q._caql"

#   collector {
#     id = "/broker/1490"
#   }

#   caql {
#     query = "op:div{find:average(\"transaction - server_error_count - all\",\"and(host:all,method:GET,service:${each.key})\")|fill(0)|stats:sum()|window:sum(6h,period=1h),find:average(\"transaction - request_count - all\",\"and(host:all,method:GET,service:${each.key})\")|fill(1)|stats:sum()|window:sum(6h,period=1h)}|op:prod(100)|label(\"burn_rate_6hour\")"
#   }

#   metric {
#     name = "burn_rate_6hour"
#     type = "numeric"
#     active = true
#     unit = "%"
#   }

#   tags = [
#     "service:${lower(each.key)}"
#   ]
# }

// ---------------------- rulesets to alert on response error burn rate


# resource "circonus_rule_set" "error_five_minute_burn" {
#   for_each = {for k,v in local.sre_errors_services: k => v if v.severity != null}
#   check = circonus_check.caql_five_minute_error_rate[each.key].checks[0]
#   metric_name = "burn_rate_5min"
#   notes = "This service is burning through its response error budget very quickly"
#   link = "https://${each.value.url_base}.circonus.com/trending/graphs/view/${element(split("/", circonus_graph.service_errors_slo_graph[each.key].id),2)}"

#   if {
#     then {
#       severity = each.value.severity
#       after    = "360"
#     }
#     value {
#       over {
#         last = "60"
#         using = "average"
#       }
#       max_value = "10"
#     }
#   }
#   tags = [
#     "service:${lower(each.key)}"
#   ]
# }

# resource "circonus_rule_set" "error_thirty_minute_burn" {
#   for_each = {for k,v in local.sre_errors_services: k => v if v.severity != null}
#   check = circonus_check.caql_thirty_minute_error_rate[each.key].checks[0]
#   metric_name = "burn_rate_30min"
#   notes = "This service is burning through its response error budget moderately quickly"
#   link = "https://${each.value.url_base}.circonus.com/trending/graphs/view/${element(split("/", circonus_graph.service_errors_slo_graph[each.key].id),2)}"

#   if {
#     then {
#       severity = each.value.severity
#       after    = "360"
#     }
#     value {
#       over {
#         last = "60"
#         using = "average"
#       }
#       max_value = "20"
#     }
#   }
#   tags = [
#     "service:${lower(each.key)}"
#   ]

# }

# resource "circonus_rule_set" "error_one_hour_burn" {
#   for_each = {for k,v in local.sre_errors_services: k => v if v.severity != null}
#   check = circonus_check.caql_one_hour_error_rate[each.key].checks[0]
#   metric_name = "burn_rate_1hour"
#   notes = "This service is burning through its response error budget moderately"
#   link = "https://${each.value.url_base}.circonus.com/trending/graphs/view/${element(split("/", circonus_graph.service_errors_slo_graph[each.key].id),2)}"

#   if {
#     then {
#       severity = each.value.severity
#       after    = "360"
#     }
#     value {
#       over {
#         last = "60"
#         using = "average"
#       }
#       max_value = "10"
#     }
#   }
#   tags = [
#     "service:${lower(each.key)}"
#   ]
# }

# resource "circonus_rule_set" "error_six_hour_burn" {
#   for_each = {for k,v in local.sre_errors_services: k => v if v.severity != null}
#   check = circonus_check.caql_six_hour_error_rate[each.key].checks[0]
#   metric_name = "burn_rate_6hour"
#   notes = "This service is burning through its response error budget slowly"
#   link = "https://${each.value.url_base}.circonus.com/trending/graphs/view/${element(split("/", circonus_graph.service_errors_slo_graph[each.key].id),2)}"

#   if {
#     then {
#       severity = each.value.severity
#       after    = "360"
#     }
#     value {
#       over {
#         last = "60"
#         using = "average"
#       }
#       max_value = "10"
#     }
#   }

#   tags = [
#     "service:${lower(each.key)}"
#   ]
# }

// ---------------------- ruleset groups to notify on response error burn rate when more than 1 ruleset fails

# resource "circonus_rule_set_group" "response_burn_too_fast_short" {
#   for_each = {for k,v in local.sre_errors_services: k => v if v.severity != null}
#   name = "${each.key} - Server error 500's have spiked"

#   notify {
#     sev1 = local.sre_contacts[each.key].sev1
#     sev2 = local.sre_contacts[each.key].sev2
#     sev3 = local.sre_contacts[each.key].sev3
#     sev4 = local.sre_contacts[each.key].sev4
#     sev5 = local.sre_contacts[each.key].sev5
#   }

#   formula {
#     expression = "A and B"
#     raise_severity = each.value.severity
#     wait = 1
#   }

#   condition {
#     index = 1
#     rule_set = circonus_rule_set.error_thirty_minute_burn[each.key].rule_set_id
#     matching_severities = ["${each.value.severity}"]
#   }
#   condition {
#     index = 2
#     rule_set = circonus_rule_set.error_one_hour_burn[each.key].rule_set_id
#     matching_severities = ["${each.value.severity}"]
#   }
# }

# resource "circonus_rule_set_group" "response_burn_too_fast_long" {
#   for_each = {for k,v in local.sre_errors_services: k => v if v.severity != null}
#   name = "${each.key} - Server error 500's are elevating"

#   notify {
#     sev1 = local.sre_contacts[each.key].sev1
#     sev2 = local.sre_contacts[each.key].sev2
#     sev3 = local.sre_contacts[each.key].sev3
#     sev4 = local.sre_contacts[each.key].sev4
#     sev5 = local.sre_contacts[each.key].sev5
#   }

#   formula {
#     expression = "A and B"
#     raise_severity = each.value.severity
#     wait = 1
#   }

#   condition {
#     index = 1
#     rule_set = circonus_rule_set.error_one_hour_burn[each.key].rule_set_id
#     matching_severities = ["${each.value.severity}"]
#   }
#   condition {
#     index = 2
#     rule_set = circonus_rule_set.error_six_hour_burn[each.key].rule_set_id
#     matching_severities = ["${each.value.severity}"]
#   }
# }

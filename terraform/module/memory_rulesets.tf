resource "circonus_rule_set" "service_memory" {
  for_each = {for k,v in var.top-level-services: k => v if v.sre.memory_alert_threshold != null}
  check = "/check/0"
  metric_pattern = each.value.runtime == "java" ? "jvm - memory - heap - used" : "nodejs - memory - heap - used"
  metric_filter = "and(service:${each.value.name},host:all,ip:all)"
  notes = "The service average heap utilization is over the threshold.  Leak?"

  if {
    then {
      severity = each.value.sre.memory_alert_severity
      after    = "300"
      notify   = (each.value.sre.memory_alert_severity == 1 ? local.sre_contacts[each.key].sev1 :
        each.value.sre.memory_alert_severity == 2 ? local.sre_contacts[each.key].sev2 :
        each.value.sre.memory_alert_severity == 3 ? local.sre_contacts[each.key].sev3 :
        each.value.sre.memory_alert_severity == 4 ? local.sre_contacts[each.key].sev4 :
        local.sre_contacts[each.key].sev5)
    }
    value {
      over {
        last = "300"
        using = "average"
      }
      max_value = each.value.sre.memory_alert_threshold
    }
  }
  tags = [
    "service:${lower(each.key)}"
  ]
}

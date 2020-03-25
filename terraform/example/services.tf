module "circonus" {

  source = "../module"
  circonus_api_key = var.circonus_api_key
  team_name = "myteam"
  sre = {
    sev1 = []
    sev2 = ["/contact_group/12345"]
    sev3 = []
    sev4 = []
    sev5 = []
  }

  top-level-services = {
    "myteam-myservice-myenvironment" = {
      name = "myteam-myservice-myenvironment"
      type = "transactional"
      runtime = "nodejs"
      sre = {
        memory_alert_threshold = null
        memory_alert_severity = null
        override = null

        latency = {
          description = "The service will respond to 99% of requests in at or under 1000ms.  Error budget covers 4 weeks."
          window_threshold_ms = 1000
          url_base = "myteam"
          window_percentile = 99
          budget_duration = "4w"
          budget_timezone = "US/Eastern"
          severity = 2
        }
        errors = null
      }
    },
  }
  urls = [
  ]
  apm_check_uuid = "<some check uuid>"
  apm_check_cid = "<some check bundle cid>"
}


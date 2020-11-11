#
# See README.md in this directory
#
variable "top-level-services" {
  type   = map(object({
    name = string
    type = string
    runtime = string
    sre  = object({
      memory_alert_threshold = number
      memory_alert_severity = number
      override = object({
        sev1 = list(string)
        sev2 = list(string)
        sev3 = list(string)
        sev4 = list(string)
        sev5 = list(string)
      })
      latency = object({
        description = string
        window_threshold_ms = number
        url_base = string
        window_percentile = number
        budget_duration = string // like 1w, 4w, 2d, etc..
        budget_timezone = string // like "US/Eastern"
        severity = number
      })
      errors = object({
        description = string
        url_base = string
        window_percentile = number
        budget_duration = string // like 1w, 4w, 2d, etc..
        budget_timezone = string // like "US/Eastern"
        severity = number
      })
    })
  }))
}

variable "sre" {
  type = object({
    sev1 = list(string)
    sev2 = list(string)
    sev3 = list(string)
    sev4 = list(string)
    sev5 = list(string)
  })
}

variable "urls" {
  type    = list
}

variable "apm_check_uuid" {
  type    = string
}

variable "apm_check_cid" {
  type = string
}

variable "team_name" {
  type = string
}

variable "circonus_api_key" {
  type    = string
}

variable "circonus_color_palette" {
  type  = list
  default = [
    "#4C8BCC",
    "#8FB4D5",
    "#BABDC0",
    "#4BB4D4",
    "#4064B0",
    "#9E75AD"
  ]
}

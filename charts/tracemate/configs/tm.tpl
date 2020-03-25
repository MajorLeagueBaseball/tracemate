<?xml version="1.0" encoding="utf8" standalone="yes"?>
<tm lockfile="/var/run/tm.lock" text_size_limit="512">
  <watchdog glider="/tracemate/backwash" tracedir="/tracemate/data/traces" retries="1" span="0" />
  <eventer>
    <config>
      <concurrency>2</concurrency>
      <default_queue_threads>2</default_queue_threads>
      <ssl_dhparam1024_file></ssl_dhparam1024_file>
      <ssl_dhparam2048_file></ssl_dhparam2048_file>
    </config>
  </eventer>
  <logs>
    <!--<log name="logfile" type="file" path="{{ .Values.tracemate.log_file_path }}" rotate_bytes="10000000" retain_bytes="50000000" timestamps="on" />-->
    <log name="internal" type="memory" path="10000,100000"/>
    <console_output>
      <outlet name="stderr"/>
      <outlet name="internal"/>
      <!-- <outlet name="logfile"/> -->
      <log name="error"/>
      <log name="debug/jaeger" disabled="true"/>
      <log name="debug/mlb" disabled="false"/>
      <log name="error/mlb" disabled="false"/>
    </console_output>
  </logs>
  <listeners>
    <consoles type="mtev_console">
      <listener address="/tmp/tm">
        <config>
          <line_protocol>telnet</line_protocol>
        </config>
      </listener>
    </consoles>
    <listener type="http_rest_api" address="*" port="8888" ssl="off">
      <config>
        <document_root>/tracemate/tm-web</document_root>
      </config>
    </listener>
  </listeners>
  <rest>
    <acl>
      <rule type="allow" />
    </acl>
  </rest>
  <kafka>
    <broker_list>{{ join "," .Values.kafkaProperties.nodes  }}</broker_list>
    <group_id>{{ .Values.kafkaProperties.kafka_group_id }}</group_id>
    <topics>
      {{ range $partition := .Values.kafkaProperties.kafka_partitions }}
      <topic name="elastic_apm_metric" partition="{{ $partition }}" batch_size="1000"/>
      <topic name="elastic_apm_transaction" partition="{{ $partition }}" batch_size="5000"/>
      <topic name="elastic_apm_error" partition="{{ $partition }}" batch_size="1000"/>
      <topic name="elastic_apm_span" partition="{{ $partition }}" batch_size="5000"/>
      <topic name="tracemate_aggregates" partition="{{ $partition }}" batch_size="1000"/>
      {{ end }}
    </topics>
  </kafka>
  <teams>

    <!--
        "metric_submission_url" is where we send the synthesized metric JSON for this team (Circonus)
        "jaeger_dest_url" is where we send trace data (gRPC protobuf)
    -->
  {{ range $key, $value := .Values.tracemate.teams }}
    <team name="{{ $key }}"
          metric_submission_url="{{ $value.circonus_url }}"
          jaeger_dest_url="{{ $value.jaeger_dest_url }}">
      <path_allowlist>
        {{ range $value.path_allowlist }}
        <path>{{ . }}</path>
        {{ end }}
      </path_allowlist>
      <path_rewrites>
        {{ range $v := $value.path_rewrites }}
        <regex regex="{{ $v.regex }}" replace="{{ $v.replace }}" />
        {{ end }}
      </path_rewrites>
    </team>
  {{ end }}
  </teams>

 {{ if .Values.module -}}
  <modules directory="/tracemate/modules">
    <generic image="{{ .Values.module.image }}" name="{{ .Values.module.name }}">
    </generic>
  </modules>

  <mlb>
    {{ range $fieldname, $fieldvalue := .Values.module.config -}}
    {{ range $fname, $fvalue := $fieldvalue -}}
    <{{$fname}}>
      {{ range $xname, $xvalue := $fvalue -}}
        <{{$xname}}>{{$xvalue}}</{{$xname}}>
      {{ end }}
    </{{$fname}}>
    {{ end }}
    {{ end }}
  </mlb>
 {{ end -}}


  <transaction_db path="{{ .Values.tracemate.local_db_path }}" initial_size="{{ .Values.tracemate.local_db_size }}" lookback_seconds="{{ .Values.tracemate.trans_lookback_seconds }}" />
  <circonus_journal_path>{{ .Values.tracemate.circonus_journal_path }}</circonus_journal_path>
  <consul_token>{{ .Values.tracemate.consul_token }}</consul_token>
  <infra_dest_url>{{ .Values.tracemate.infra_dest_url }}</infra_dest_url>
</tm>

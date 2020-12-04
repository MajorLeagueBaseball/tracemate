
# tracemate
A C application that reads Elastic APM data from kafka and does things with the
data.

This app synthesizes metrics from the Elastic APM trace data and emits as JSON
suitable for sending to Circonus HTTPTrap.

It also compares APM transaction latency to a threshold and converts the spans
to Jaeger format and sends to Jaeger if the transactions are longer than the
time threshold (or if there is a reported error with the transaction).

## Running

Pre-built images are hosted on [Docker hub](https://hub.docker.com/r/majorleaguebaseball/tracemate).  You can:

```
docker run majorleaguebaseball/tracemate:<release>
```

However, you are likely to need proper configuration first. See `src/tm.conf.in`
and `Configuration` in this document. Once your configuration is complete you
will want to mount it. The default docker container looks for its configuration
at `/tracemate/tm.conf` so once you produce a config file, mount into the
container:

```
docker run -v my.conf:/tracemate/tm.conf majorleaguebaseball/tracemate:<release>
```

To run and use your own config.

## Building

You can:

```
docker build .
```

In the root folder to produce a docker image. Or you can use the
`buildtools/docker.sh` script to produce a local dev container for building in.
This dev container mounts the source tree directly into the container so you can
edit source outside of the container (or inside) and compile it in the container
(and then run it in the container too).

```
buildtools/docker.sh --build
...
buildtools/docker.sh --shell
```

From within the container you can:

```
riley.berton $ buildtools/docker.sh --shell
Running tracemate container image
[root@tracemate tracemate]# autoreconf -i
[root@tracemate tracemate]# ./configure
***********************************************************
* Loading node specific configure settings for tracemate.
***********************************************************
checking build system type... x86_64-unknown-linux-gnu
checking host system type... x86_64-unknown-linux-gnu
...
[root@tracemate tracemate]# make
```

tracemate supports a local `.configure.tracemate` file into which you can stick
specialized configure settings which will get picked up during the `./configure`
step. Example:

```
[root@tracemate tracemate]# cat .configure.tracemate
export CFLAGS="$CFLAGS -O0 -ggdb -DNO_PUBLISH=1"
export CXXFLAGS="$CXXFLAGS -O0 -ggdb -DNO_PUBLISH=1"
```
> Turn off optimizations, and set the NO_PUBLISH flag.

## Design

Tracemate was built to interact with a portion of the [Elastic
APM](https://www.elastic.co/apm) stack (which is an open source APM stack).
Elastic APM was originally designed to work with Elastic Search for document
storage and then build dashboards and search functionality on top of Kibana to
visualize APM data. If you want to have highly accurate latency measurements in
your APM data then using Elastic Search can quickly get very expensive since you
would be paying to store (and index!) every transaction and span document into
Elastic Search. Tracemate takes a different approach.

Elastic APM server can be configured to output its documents to a Kafka cluster
instead of (or in addition to) storing them in Elastic Search. This was intially
meant as a way to deal with high volume ingestion spikes but Tracemate exploits
this to bypass the need for Elastic Search at all.

Tracemate reads the topics produced by Elastic APM server and does 2 things:

1. Synthesize metrics from elastic APM documents and store the resultant metrics
   in Circonus.
2. Synthesize distributed traces from elastic APM documents and store the
   resultant traces in Jaeger.

If you combine these 2 features with the already supported [log
correlation](https://www.elastic.co/guide/en/apm/agent/java/1.x/log-correlation.html)
feature of Elastic APM, you get all 3 legs of the stool of observability at an
affordable price.

### Synthesize Metrics

To accomplish feature "1", tracemate uses a familiar pattern of data
republication over kafka to ensure that data arrives at a single tracemate
instance to be aggregated correctly. The pattern looks like this:

```
+--------------+
|              |
|              | 1.read elastic APM     +-----------------+
|              +----------------------> |                 |
|              |                        |                 |
|              |                        |  tracemate 1    |
|   Kafka      | 2.republish w/agg key  |                 |
|              | <----------------------+                 |
|              |                        +-----------------+
|              |
|              |
|              |                        +-----------------+                     +------------------+
|              | 3.read agg keyed data  |                 |                     |                  |
|              +----------------------->+                 |  4. publish         |                  |
|              |                        |  tracemate N    | +------------------>+    circonus      |
|              |                        |                 |                     |                  |
|              |                        |                 |                     |                  |
|              |                        +-----------------+                     +------------------+
|              |
+--------------+

```

Each aggregatable metric is republished using it's full metric name (with tags)
as the key into a kafka topic called "tracemate_aggregates". This key causes the
metric to be hashed onto one of the topic's configured partitions. Some other
(or maybe the same) tracemate instance then reads this aggregate topic and
aggregates the data associated with that metric name. For histogram metrics,
this is a merge. For average style metrics this is a count and a sum. Since
kafka is durable, this pattern survives crashes and restarts of tracemate
instances. They will merely pick up from their last position and continue to
aggregate.

Some data that arrives at tracemate is not aggregated (the `elastic_apm_metric`
topic). This data is sent on directly to Circonus.

### Synthesize Distributed Traces

To accomplish feature "2" it's more complicated. Tracemate maintains a local
LMDB database on disk to house the incoming documents until a tracing decision
can be made about them. This tail based sampling approach allows us to keep most
of the noise out of Jaeger and therefore save on storage costs to house all the
trace data. As transactions and spans arrive they are stored in this local
embedded database until a triggering event happens which would cause the data to
be sent on to Jaeger. There are currently 2 triggering events:

1. an `elastic_apm_error` document arrives that is associated to the transaction
2. an `elastic_apm_transaction` document arrives that has a latency longer than
   the threshold for that service (or longer than the default if the service has
   no configured threshold).

If either `1` or `2` happens, we send the transaction, all collected spans, all
collected related transactions from other services, and all collected errors on
to jaeger for that service.

Periodically, this local database is pruned of older transactions that are not
going to be traced (or have already been traced) to keep the size down.

## Requirements

### Elastic APM Agent

Elastic APM Agent is available for [many
languages/runtimes](https://www.elastic.co/guide/en/apm/get-started/current/components.html#_apm_agents).
We will focus on the config for the [Java
agent](https://www.elastic.co/guide/en/apm/agent/java/1.x/intro.html) here but
the process is similar for other languages/runtimes.

Download and place the elastic APM jar file onto the file system somewhere.
Then:

```
java -javaagent:/path/to/elastic-apm-agent-<version>.jar \
  -Delastic.apm.service_name=myteam-my-cool-service \
  -Delastic.apm.application_packages=org.example,org.another.example \
  -Delastic.apm.server_urls=http://your-apm-server:8200 \
  -Delastic.apm.transaction_sample_rate=1.0 \
  -jar your-application.jar
```

Note that tracemate uses the `elastic.apm.service_name` to steer data to the
proper owning `<team>` (and therefore the Circonus account and jaeger instance
for the related data). See the `Configuration` section below for more info on
how this works. Also, `elastic.apm.transaction_sample_rate` must be set to 1.0
(this is the default and is only included here for clarity). This setting is
important so that we can calculate accurate latency information for transactions
and spans. Please also read the [elastic apm agent configration
docs](https://www.elastic.co/guide/en/apm/agent/java/1.x/configuration.html) to
understand all the possible switches to use with the agent.

When you run the above it will cause your process to spit out metrics,
transactions, spans, and errors to the Elastic APM Server running at
`http://your-apm-server:8200`. Tracemate will only support whatever Elastic APM
Agent already supports. If you are using a hand-rolled http library or servlet
context, you will likely see no data (unless you fold in specific programmatic
transactions and spans into your code). More libraries are being supported all
the time so if you don't see your particular library you can request it from
Elastic. [The list of supported technologies for
java](https://www.elastic.co/guide/en/apm/agent/java/1.x/supported-technologies-details.html)


### Elastic APM Server

Elastic APM server must be configured to output its documents to
Kafka with a specific config:

```
cat >> /etc/apm-server/apm-server.yml <<'EOF'
#------------------------------- Kafka output ----------------------------------
output.kafka:
  enabled: true

  hosts: [
     "kafka.server1:9200",
     "kafka.server2:9200"
  ]
  topic: 'elastic_apm_%{[processor.event]}'

  # The Kafka event partitioning strategy. Default hashing strategy is `hash`
  # using the `output.kafka.key` setting or randomly distributes events if
  # `output.kafka.key` is not configured.
  partition.hash:
    # If enabled, events will only be published to partitions with reachable
    # leaders. Default is false.
    reachable_only: true

    # Configure alternative event field names used to compute the hash value.
    # If empty `output.kafka.key` setting will be used.
    # Default value is empty list.
    hash: ["trace.id"]

  # The maximum number of events to bulk in a single Kafka request. The default
  # is 2048.
  bulk_max_size: 150

  # The keep-alive period for an active network connection. If 0s, keep-alives
  # are disabled. The default is 0 seconds.
  keep_alive: 120

  # Sets the output compression codec. Must be one of none, snappy and gzip. The
  # default is gzip.
  compression: none

  # The ACK reliability level required from broker. 0=no response, 1=wait for
  # local commit, -1=wait for all replicas to commit. The default is 1.  Note:
  # If set to 0, no ACKs are returned by Kafka. Messages might be lost silently
  # on error.
  required_acks: 1
EOF
```

This splits data into 5 topics: `elastic_apm_error`, `elastic_apm_metric`,
`elastic_apm_onboarding`, `elastic_apm_span`, and `elastic_apm_transaction`.
(`onboarding` is just a startup topic which the apm servers write to. It's not
used in normal processing by tracemate)

> Importantly, we use `trace.id` as the `hash` key. This guarantees that related
> documents (transaction and span) are always published to the same partition
> based on the hash of the `trace.id`. This, by extension, ensures that the same
> tracemate instance always processes all related documents for a trace. This
> helps us get the metric aggregation correct as well as the distributed trace
> data complete.

### Kafka

The next requirement is a working kafka cluster. With topics created and
partitioned properly for your expected volume of trace data. Recall from the
`Elastic APM Agent` section that we are tracing at 100% probability (the
default) in order to get very accurate latency metrics produced. So size your
cluster appropriately.

@MLB we use a 10 node `n1-standard-4` cluster (on Google Cloud) with 500GB SSD
persistent disks and a topic replication factor of 2 where each topic has 100
partitions. Over the past 30 days, we haven't see any node go above 15% cpu or a
write rate over 1MB/second on any individual node. We are purposely sized for 5X
headroom in operations to deal with expected (and unexpected growth). Size
yourself according to your needs.

The kafka cluster will require the topics mentioned in the last section as well
as a few specialized topics: `tracemate_aggregates`, `tracemate_urls`, and
`tracemate_regexes`. These topics are used to centralize aggregate math properly
and to squash URLs. Once an aggregate key is created for a metric, it is
republished to the `tracemate_aggregates` topic with the metric name as the key
so that it will hash to a single partition for aggregation. This ensures that
the same tracemate instance is always aggregating the same metrics by key all
the time (it keeps the math coherent).

Tracemate uses service URL for several metrics (`transaction - latency - <URL>`,
etc..). This URL is directly derived from the elastic apm reported
`request.url.pathname`. The first pass of tracemate utilized a set of regular
expressions that it compared to URL path segments in order to replace known
patterns like UUIDs or integers with a named replacement like `/foo/bar/1234`
would become `/foo/bar/{id}`. This didn't scale well when the URLs processed by
tracemate became less well formed. For example:

`/news/buffalo-bluejays/toronto-blue-jays-move-to-buffalo-for-the-covid-season.html`

Human readable URLs fell down horribly when using a regex approach and it led to
metric grouping issues.

This code uses a different approach. Tracemate now analyzes every incoming
URL that it sees (per service) and assigns a cardinality score to each path
segment tree. When the cardinality eclipses a logarithmically decreasing
threshold (based on the segment number), the segment is squashed and replaced
with `{...}`. So the above URL becomes:

`/news/buffalo-bluejays/{...}`

If there are enough news stories to eclipse the cardinality limit.

To accomplish this feat, tracemate makes use of 2 more kafka topics:

1. tracemate_urls
2. tracemate_regexes

Every URL that tracemate sees is immediately published to `tracemate_urls` with
the key of the service name so that it gets processed by the owner of that
partition (this ensures that all URLs for a service get processed by the same
tracemate instance). When a URL becomes squashed tracemate produces a regular
expression that is publishes to `tracemate_regexes`. All tracemate instances
subscribe to `tracemate_regexes` to update their internal URL replacement
schemes when new regexes are produced.

`tracemate_urls` should likely match the number of partitions you use for
`elastic_apm_*` topics and `tracemate_regexes` should have 1 partition.


To re-iterate we need 8 topics:

* `elastic_apm_error` - holds error documents reported by elastic apm agent.
* `elastic_apm_metric` - holds metric documents reported by elastic apm agent.
* `elastic_apm_onboarding` - used by elastic apm server during startup, not used
  by tracemate but required to exist. This topic can have 1 partition.
* `elastic_apm_span` - holds span documents (components of a transaction)
  reported by elastic apm agent.
* `elastic_apm_transaction` - holds transaction documents reported by elastic
  apm_agent.
* `tracemate_aggregates` - holds aggregate documents produced by tracemate
  itself so we produce coherent math.
* `tracemate_urls` - holds each seen URL per service.
* `tracemate_regexes` - holds the result of the URL squashing code.

6 of these (all but `elastic_apm_onboarding` and `tracemate_regexes`) **must**
have the same number of partitions.  

* `tracemate_regexes` - must have 1 partition with the Kafka setting:
`cleanup.policy` set to `compact` so that duplicate regexes get compacted away.
@MLB we also set `delete.retention.ms=300000` and `segment.ms=10000`
* `elastic_apm_onboarding` - doesn't seem to matter but we use 1 partition here.


### Jaeger

There are no special requirements on Jaeger, simply that it exists. We run
jaeger on k8s using scylladb as the backend. See the `charts/jaeger-instance`
Helm chart for an example deployment.

https://www.jaegertracing.io/

## Configuration

Tracemate uses [libmtev](https://github.com/circonus-labs/libmtev) as its base
application framework. As such, it inherits libmtev's XML based configuration
syntax [which you can read more about
here](http://circonus-labs.github.io/libmtev/config/). Tracemate specific config
is as follows (it helps to read the `Design` section of this document first and
see the example `src/tm.conf.in`):

### Kafka
```
  <kafka>
    <broker_list>your.broker1.here:9092,your.broker2.here:9092</broker_list>
    <group_id>tracemate_subscription</group_id>
    <topics>
      <topic name="elastic_apm_metric" partition="0" batch_size="100" read_delay_ms="3000" />
      <topic name="elastic_apm_transaction" partition="0" batch_size="2000"/>
      <topic name="elastic_apm_error" partition="0" batch_size="100"/>
      <topic name="elastic_apm_span" partition="0" batch_size="2000"/>
      <topic name="tracemate_aggregates" partition="0" batch_size="100"/>
      <topic name="tracemate_urls" partition="0" batch_size="100"/>
      <topic name="tracemate_regexes" partition="0" batch_size="100"/>
    </topics>
  </kafka>
```

This section configures the kafka brokers that tracemate should connect to, the
subscription name (`group_id`) so that kafka can maintain the offset and the
list of topic/partitions (and related settings). A tracemate instance can
connect to any number of topic/partitions depending on the number of instances
you want to run. As an example, MLB runs 20 instances of tracemate each reading
from 5 partitions of each topic. Each topic is configured to have 100
partitions.

### Teams
```
  <teams>
    <team name="myteam" metric_submission_url="https://api.circonus.com/module/httptrap/c10874ff-63a2-4f74-8a2d-0c2e5b87be5b/mys3cr3t" jaeger_dest_url="your-jaeger-collector:14250" collect_host_level_metrics="false" circonus_api_key="example-api-key" check_id="example-numeric-check-id-associated-with-http-trap-check">
      <path_allowlist>
        <path>/</path>
      </path_allowlist>
      <path_rewrites>
        <regex regex="(\/[0-9]+(\/|$))" replace="/{id}/" />
        <regex regex="(\/[a-fA-F0-9]{8}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{12}(-ext)?(\/|$))" replace="/{guid}/" />
        <regex regex="(\/?[\w@]+[ \.-]{1,}[^\/]+)$" replace="/{file}" />
      </path_rewrites>
    </team>
  </teams>
```

The teams list controls how data is separated between services and which
httptrap URL the metrics are sent to for the team, as well as the jaeger
collector service to send distributed traces to for the team. @MLB we are split
into rougly 8 to 10 teams which span the various codebases throughout baseball.
For example, we have a Baseball Data team (bdata), a Baseball Services team
(bbserv), an enterprise services team (best), etc.. Each of these teams has
their own Circonus account to keep the data separate and their own jaeger
instance to keep tracing separate. It is not a strict requirement to split up
your data in this way. You could easily have a single team, possibly named after
your organization, with a single httptrap to send data to and a single jaeger
collector to send traces. This would co-mingle all your organization's data into
one circonus account and one jaeger instance.

> Importantly, the services as configured in `Elastic APM Agent` must have the team prefix in order to be matched with the team configuration in the
> configuration file.  If you use an organization model and your <team> is called: "acme", your service names must be prefixed with: "acme-"

Each `<team>` definition has 4 important member fields, and two optional fields:

* `metric_submission_url` - the Circonus HTTPTrap check URL to send synthesized
  metrics to
* `jaeger_dest_url` - the jaeger collector host/port where the jaeger gRPC
  listener is running to send jaeger trace data
* `circonus_api_key` - An API key that will be used to generate graphs and
  dashboards.
* `check_id` - The numeric check id associated with the `metric_submission_url`.
  this can be found under the https://account-name.circonus.com/checks?type=httptrap
  address, and will typically be a six digit integer. This is used for graph 
  generation.
* `collect_host_level_metrics` - Defaults to "false". If set to "true" will
  create metrics that track data at the host level. Specifically, this means
  that for every metric generated (see `Metrics` below), tracemate will generate
  a `host:` tag on the metric that ties the data to the specific host which
  produced it. Turning this on will inflate the cardinality of the metrics
  produced but can help illuminate differences in performance among different
  hosts. This is likely less useful in a k8s environment where pods are jumping
  around so we leave this off @MLB.
* `rollup_high_cardinality` - Defaults to "false". Certain metrics can be
  very high cardinality.  For example: `transaction - latency - /level1/level2/...`
  where you have a `path_rewrites` set to only allow 2 levels of URL pathing.
  This can still mean many millions of time series depending on the service.
  To combat this explosion of time series, Circonus offers a special tag
  which will keep the metric for 4 weeks but then drop it for longer term
  storage (won't roll it up).  Setting this to true for a team will
  omit this special tag causing Circonus to rollup everything.
  
Each team can have a `path_allowlist` and a `path_rewrites` list to control
which URLs you pay attention to in your services. To allow all paths, simply
create a single path of slash `/` as in the example above. If you don't specify
anything in `path_allowlist`, tracemate will ignore all transactions for the
team.

`path_rewrites` allow you to perform regexes over URLs reported by Elastic APM
and genericize them to make metric aggregation more useful. You can see in the
example above where we are replacing all integers in a path (or at the end of a
path) with `{id}`, all GUIDs with `{guid}` and anything that looks like a file
with an extension with `{file}`. If a URL then arrived that resembled:

```
/foo/bar/123/test.json
```

It would get re-written as:

```
/foo/bar/{id}/{file}
```

If another URL arrived like:

```
/foo/bar/456/test.json
```

It would also get re-written as:

```
/foo/bar/{id}/{file}
```

Therefore aggregating together all related metrics under this single re-written
URL.

> AIUI the transaction naming has gotten better in Elastic APM 7.X so this
> feature may be less necessary going forward in favor of just relying on
> whatever Elastic APM decides to name the transaction.

### Service Thresholds

```
  <service_thresholds>
    <threshold name="myteam-myservice-myenvironment" threshold_ms="1500" />
    <threshold name="default" threshold_ms="2000" />
  </service_thresholds>
```

`<service_thresholds>` let you configure the time over which a transaction is
sent to jaeger for tracing. This should contain a list of `<threshold>`s where
the `name` parameter matches the name in the Elastic APM Agent configuration for
`elastic.apm.service_name` and the `threshold_ms` parameter contains the time in
milliseconds.

You can set a special key called "default" which will be used as the default
threshold for all services that aren't otherwise configured.

> If you don't specify any thresholds, tracemate uses a default of 2000ms as the threshold.

### Transaction DB

```
  <transaction_db path="/tracemate/data/ts" />
```

The file system location of the local LMDB database used to temporarily house
related trace documents. In the k8s world (see `charts/tracemate`), we rely on a
persistent volume claim mounted at `/tracemate/data` in order to persist this
local database across container deployments and restarts.

### Journal path
```
  <circonus_journal_path>/tracemate/data/journal</circonus_journal_path>
```

The file system location of the write ahead log for sending data to circonus.
Tracemate uses a store and forward approach with checkpointing to ensure no data
gets lost before it successfully arrives to Circonus. Like the `Transaction DB`
we use the same persistent volume claim to ensure this journal survives
container moves and restarts.

### Infra dest URL
```
   <infra_dest_url>https://api.circonus.com/module/httptrap/GUID/mys3cr3t</infra_dest_url>
```

This is a holdover name from early development. What it represents is the
HTTPTrap URL where tracemate's internal metrics should be sent as JSON. If you
have a single Circonus account this could be the same HTTPTrap you send your
`<team>` metrics to.


### Modules

Like all `libmtev` apps, tracemate has the ability to load
[modules](http://circonus-labs.github.io/libmtev/config/modules.html) which can
perform specific operations that aren't part of the core tracemate
functionality. To see what hooks are exposed for this purpose, you can look in
`src/tm_hooks.h`. More hooks will be added over time.

## Metrics

The generated metrics follow a naming pattern:

```
{type} - {metric} - {identifier}|ST[tagset]
```

Where:

- `{type}` is `transaction`, `db`, or `external`

- `{metric}` is the thing we are tracking, examples include: `latency`,
  `error_count`, `request_count`, etc..

- `{identifier}` is the path or SQL info, examples include: `/foo/bar/{id}`,
  `db.mysql.query foo SELECT`, etc.. `[tagset]` is the set of tags that further
  identify the resource, examples: `service:best-BATTER-service`, `host:foo.mlb.com`,
  `ip:1.2.3.4`


Examples of metrics:

```
transaction - latency - /foo|ST[service:best-BATTER-service,host:foo.mlb.com,ip:1.2.3.4]
transaction - request_count - /foo|ST[service:best-BATTER-service,host:foo.mlb.com,ip:1.2.3.4]
db - latency - /foo|ST[service:best-BATTER-service,host:foo.mlb.com,ip:1.2.3.4]
```

There are special rollup metrics created for the host where the specific host
is replaced with `all`.. Examples:

```
transaction - latency - /foo|ST[service:best-BATTER-service,host:all,ip:all]
transaction - request_count - /foo|ST[service:BATTER-service,host:all,ip:all]
db - latency - /foo|ST[service:BATTER-service,host:all,ip:all]
```

These let us look at a specific URL across hosts for a rolled up view.

This app also generates even higher level rollup metrics for ease in graphing
and alerting. For rollup metrics, the `{identifier}` portion will be replaced
with `all` and the tags that are specific to a host will be replaced with `all`

Examples of aggregated metrics:

```
transaction - latency - all|ST[service:BATTER-service,host:all,ip:all]
transaction - request_count - all|ST[service:BATTER-service,host:all,ip:all]
db - latency - all|ST[service:BATTER-service,host:all,ip:all]
```

This app produces the following metrics (not all metrics apply to all `{type}s`):

- `latency` a log linear histogram of every transaction/operation seen
- `average_latency` the mean of the `latency` metric stored as a single number
- `min_latency` the p0 of the `latency`
- `max_latency` the p100 of the `latency`
- `stddev_latency` the std dev of the `latency`
- `request_count` the count of ops/trans
- `client_error_count` the count where status_code on the transaction is >= 400 and < 500
- `server_error_count` the count where status_code on the transaction is >= 500 and < 600
- `error_count` the sum of the last 2
- `stmt_count` for `db` `{type}` the count of statements executed
- `call_count` for `external` `{type}` the count of external calls made

The preceding are each generated for each level in the metric naming scheme..
using `latency` as an example where we have URL's `/foo/bar/{id}` and
`/baz/bing` and hosts: `foo.mlb.com` and `bar.mlb.com`, and no database
interactions, you will end up with the following metrics:

```
transaction - latency - /foo/bar/{id}|ST[service:foo-service,host:foo.mlb.com,ip:1.2.3.4]
transaction - latency - /foo/bar/{id}|ST[service:foo-service,host:bar.mlb.com,ip:1.2.3.5]
transaction - latency - /foo/bar/{id}|ST[service:foo-service,host:all,ip:all]

transaction - latency - /baz/bing|ST[service:foo-service,host:foo.mlb.com,ip:1.2.3.4]
transaction - latency - /baz/bing|ST[service:foo-service,host:bar.mlb.com,ip:1.2.3.5]
transaction - latency - /baz/bing|ST[service:foo-service,host:all,ip:all]

transaction - latency - all|ST[service:foo-service,host:all,ip:all]
```

So the generated cardinality follows: `metric * host_count * endpoint_count + 2`
And then an `all` category with one of each metric. These apply per service.

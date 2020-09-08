# Copyright 2020 Major League Baseball
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# centos7 base image with required compilation tools and libraries
FROM majorleaguebaseball/tracemate-base:3477f29 AS base

RUN mkdir -p /tracemate

ADD . /tracemate

WORKDIR /tracemate

ARG EXTRA_CFLAGS=""
ENV EXTRA_CFLAGS=$EXTRA_CFLAGS

RUN autoreconf -i && \
  PATH=$PATH:/usr/local/bin:/opt/circonus/bin:/usr/bin \
  LDFLAGS="-L/opt/circonus/lib -L/usr/lib64/ -L/usr/local/lib" \
  CFLAGS="-I/usr/local/lib -I/opt/circonus/include -I/usr/include/librdkafka -O2 -ggdb $EXTRA_CFLAGS" \
  CXXFLAGS="-I/usr/local/lib -I/opt/circonus/include -I/usr/include/librdkafka -O2 -ggdb -std=c++11 $EXTRA_CFLAGS" \
  ./configure && \
  make clean && \
  make

# start over from a clean OS to keep the image small
FROM centos:7

RUN  yum install -y https://dl.fedoraproject.org/pub/epel/epel-release-latest-7.noarch.rpm && \
     yum install -y htop jq openssl curl openssl-libs openssl-devel less && \
     yum clean all && \
     rm -rf /var/cache/yum

RUN mkdir -p /tracemate/logs
RUN mkdir -p /tracemate/modules
RUN mkdir -p /tracemate/tm-web
RUN mkdir -p /tracemate/visuals
COPY --from=base /usr/lib64/librdkaf* /tracemate/
COPY --from=base /usr/lib64/libxslt* /tracemate/
COPY --from=base /tracemate/src/tm /tracemate/
COPY --from=base /tracemate/src/tm.conf /tracemate/
COPY --from=base /tracemate/src/tm-web/ /tracemate/tm-web/
COPY --from=base /opt/circonus/lib/ /tracemate/
COPY --from=base /tracemate/scripts/tracemate.sh /tracemate/
COPY --from=base /tracemate/visuals/* /tracemate/visuals/

CMD ["/tracemate/tracemate.sh"]
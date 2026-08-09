# YT Saurus local dev image.
# Tag: ytsaurus/local:dev
# Pin by digest for reproducible builds.
FROM ytsaurus/local:dev@sha256:1fc1aaefcae7a409d7a5c738f86b337ab268c736f2f4598c5e2bb3851ddc62ef

WORKDIR /

EXPOSE 80
EXPOSE 8443

ENTRYPOINT ["yt_local", "start", \
  "--proxy-port", "80", \
  "--rpc-proxy-port", "8443", \
  "--queue-agent-count", "1", \
  "--master-config", "/etc/yt/server.yson", \
  "--node-config", "/etc/yt/server.yson", \
  "--scheduler-config", "/etc/yt/server.yson", \
  "--controller-agent-config", "/etc/yt/controller-agent.yson", \
  "--rpc-proxy-config", "/etc/yt/server.yson", \
  "--queue-agent-config", "/etc/yt/server.yson", \
  "--local-cypress-dir", "/var/lib/yt/local-cypress", \
  "--fqdn", "localhost", \
  "--ytserver-all-path", "/usr/bin/ytserver-all", \
  "--sync"]

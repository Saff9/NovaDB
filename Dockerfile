# Dockerfile for NovaDB
# Build: docker build -t novadb .
# Run:   docker run -p 9876:9876 -v $(pwd)/data:/data novadb

FROM alpine:3.20

RUN apk add --no-cache build-base

COPY . /src
WORKDIR /src

RUN make release && mkdir -p /data

EXPOSE 9876

CMD ["./bin/novadb-server", "--data-dir", "/data", "--port", "9876", "--host", "0.0.0.0"]

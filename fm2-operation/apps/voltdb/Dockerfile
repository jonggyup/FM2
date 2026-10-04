# Use an official Ubuntu as a parent image
FROM ubuntu:20.04

# Install dependencies
RUN apt-get update && apt-get install -y \
    openjdk-8-jdk

RUN apt-get install -y python3 git ant build-essential ant-optional valgrind ntp ccache cmake

# Clone your GitHub repository
RUN git clone https://github.com/asd123www/voltdb.git /opt/voltdb

# Apply the reconnect change to the source cloned inside the image. Keeping the
# change as a patch also lets this Dockerfile work before ./voltdb is extracted.
COPY patches/enable-client-reconnect.patch /tmp/enable-client-reconnect.patch
RUN git -C /opt/voltdb apply /tmp/enable-client-reconnect.patch
COPY patches/drop-malformed-connection.patch /tmp/drop-malformed-connection.patch
RUN git -C /opt/voltdb apply /tmp/drop-malformed-connection.patch

# Set the working directory
WORKDIR /opt/voltdb

# Compile your project
RUN ant

# Set the entrypoint to bash or your compiled binary
ENTRYPOINT ["/bin/bash"]

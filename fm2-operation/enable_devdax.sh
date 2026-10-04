#!/bin/bash
sudo ndctl disable-namespace namespace0.0
sudo ndctl destroy-namespace namespace0.0
sudo ndctl create-namespace -r region0 -m devdax

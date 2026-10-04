#!/bin/bash

PID=`ps -eaf | grep backup | grep -v grep | awk '{print $2}'`
if [[ "" !=  "$PID" ]]; then
  echo "Send signal to $PID"
  kill -2 $PID
fi


PID=`ps -eaf | grep qemu | grep -v grep | awk '{print $2}'`
if [[ "" !=  "$PID" ]]; then
  echo "Send signal to $PID"
  kill -9 $PID
fi

PID=`ps -eaf | grep tpcc | grep -v grep | awk '{print $2}'`
if [[ "" !=  "$PID" ]]; then
  echo "Send signal to $PID"
  kill -9 $PID
fi


PID=`ps -eaf | grep voltdb | grep -v grep | awk '{print $2}'`
if [[ "" !=  "$PID" ]]; then
  echo "Send signal to $PID"
  kill -9 $PID
fi


PID=`ps -eaf | grep controller | grep -v grep | awk '{print $2}'`
if [[ "" !=  "$PID" ]]; then
  echo "Send signal to $PID"
  kill -9 $PID
fi

PID=`ps -eaf | grep ycsb | grep -v grep | awk '{print $2}'`
if [[ "" !=  "$PID" ]]; then
  echo "Send signal to $PID"
  kill -9 $PID
fi
screen -wipe

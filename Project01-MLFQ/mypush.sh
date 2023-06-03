#!/bin/sh

message=$*
#tips= "\""
#echo $tips
#commitee= "$tips $message $tips"
#echo $commitee
make clean
git add .
git commit -m "$message"
git push origin master

# Copyright (c) 2020      Intel, Inc.  All rights reserved.
# Copyright (c) 2020      IBM Corporation.  All rights reserved.

all: leap15 centos7 centos8

leap15: Dockerfile.leap15.ssh
	docker build -t prrte/leap15:latest -f Dockerfile.leap15.ssh .

centos7: Dockerfile.centos7.ssh
	docker build -t prrte/centos7:latest -f Dockerfile.centos7.ssh .

centos8: Dockerfile.centos8.ssh
	docker build -t prrte/centos8:latest -f Dockerfile.centos8.ssh .

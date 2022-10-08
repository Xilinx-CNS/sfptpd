/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2020 Xilinx, Inc. */

@Library('onload_jenkins_pipeline_lib')
import com.solarflarecom.onload.notifications.NotificationManager

def nm = new NotificationManager(this)

nm.slack_notify() {
  def scmVars

  node('unit-test-parallel') {
    stage('Checkout') {
      scmVars = checkout scm
      echo "Got revision ${scmVars.GIT_COMMIT}"
    }

    stage('build') {
      sh('make')
    }

    stage('check') {
      warnError('Script failed!') {
        sh('make test')
      }
    }
  }
}



/*
 ** Local variables:
 ** groovy-indent-offset: 2
 ** indent-tabs-mode: nil
 ** fill-column: 75
 ** tab-width: 2
 ** End:
 */

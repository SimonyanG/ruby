pipeline {
  agent any
  stages {
    stage('Build') {
      steps {
        sh 'autoconf'
        sh './configure'
        sh 'make'
      }
    }
    stage('Check') {
      steps {
        sh 'make check'
      }
    }
  }
}
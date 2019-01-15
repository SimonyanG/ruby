pipeline {
  agent any
  stages {
    stage('Build') {
      steps {
        sh 'autoconf'
        sh './configure'
      }
    }
  }
}
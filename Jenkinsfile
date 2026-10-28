// Sentinel Node CI/CD pipeline (Jenkins declarative).
// Layers: L1 firmware build matrix, L2 firmware host unit tests, L3 Edge Server tests,
//         L4 HIL suite (emulator dry run on every build; real node on the bench nightly / on demand),
//         mutation check (nightly), requirement traceability FW-01..FW-24.
// Setup guide: "Sentinel Node CI/CD Verification System" document.
pipeline {
    agent none
    parameters {
        booleanParam(name: 'RUN_HIL', defaultValue: false, description: 'Flash the bench node and run the hardware suite')
        booleanParam(name: 'RUN_MUTATION', defaultValue: false, description: 'Run the firmware mutation check')
        booleanParam(name: 'HIL_DESTRUCTIVE', defaultValue: true, description: 'Allow reboot / power-mode tests on the bench')
        string(name: 'HIL_NODE_IP', defaultValue: '192.168.100.44', description: 'IP address of the bench node')
        string(name: 'HIL_SERIAL_PORT', defaultValue: '/dev/ttyUSB0', description: 'USB serial port of the bench node')
        string(name: 'SOAK_MINUTES', defaultValue: '5', description: 'Data-socket stability window on the bench')
    }
    triggers {
        // nightly full run (HIL + mutation) on the main branch only
        cron(env.BRANCH_NAME == 'main' ? 'H 2 * * *' : '')
    }
    options {
        timestamps()
        timeout(time: 120, unit: 'MINUTES')
        buildDiscarder(logRotator(numToKeepStr: '30', artifactNumToKeepStr: '10'))
        disableConcurrentBuilds()
    }
    stages {
        stage('Build & test') {
            agent {
                dockerfile {
                    filename 'ci/docker/Dockerfile'
                    label 'docker'
                    args '-v sentinel-arduino-cache:/opt/arduino-cache'
                }
            }
            stages {
                stage('Static checks') {
                    steps {
                        sh 'python3 -m py_compile server/sentinel_node_api.py ci/*.py tests/common/*.py tests/fw_host/run_fw_host_tests.py tests/fw_host/framework/extract.py'
                        sh 'python3 -m pyflakes server/sentinel_node_api.py ci tests/common tests/server tests/hil'
                        sh 'cppcheck --enable=warning,performance --error-exitcode=1 --inline-suppr --std=c++17 -I firmware/sentinel_node firmware/sentinel_node/ezsn_hal.cpp 2> build-cppcheck.txt || (cat build-cppcheck.txt; exit 1)'
                    }
                }
                stage('Tests') {
                    parallel {
                        stage('L1 firmware build matrix') {
                            steps { sh 'python3 ci/build_matrix.py' }
                        }
                        stage('L2 firmware host tests') {
                            steps { sh 'python3 tests/fw_host/run_fw_host_tests.py' }
                        }
                    }
                }
                // Timing-based tests run after the CPU-heavy compile, not alongside it: under full CPU load
                // a server/emulator timing window can be missed and the result would be flaky.
                stage('L3 server + L4 HIL dry run') {
                    steps {
                        sh 'python3 -m pytest tests/server -p no:cacheprovider --timeout 120 --junitxml build/server/junit.xml --req-json build/server/results.json'
                        sh 'python3 -m pytest tests/hil --target emulator --destructive -p no:cacheprovider --timeout 300 --junitxml build/hil/junit.xml --req-json build/hil/results.json'
                    }
                }
                stage('Mutation check') {
                    when { anyOf { triggeredBy 'TimerTrigger'; expression { params.RUN_MUTATION } } }
                    steps { sh 'python3 ci/mutation_check.py --junit build/mutation/junit.xml' }
                }
                stage('Traceability') {
                    steps { sh 'python3 ci/trace_report.py' }
                }
            }
            post {
                always {
                    junit allowEmptyResults: true, testResults: 'build/firmware/junit.xml, build/fw_host/junit/*.xml, build/server/junit.xml, build/hil/junit.xml, build/mutation/junit.xml'
                    archiveArtifacts allowEmptyArchive: true, artifacts: 'build/firmware/artifacts/**, build/firmware/*/compile.log, build/trace/**'
                    publishHTML(target: [reportName: 'Traceability', reportDir: 'build/trace', reportFiles: 'traceability.html',
                                         keepAll: true, alwaysLinkToLastBuild: true, allowMissing: true])
                    stash name: 'results', includes: 'build/firmware/results.json, build/fw_host/results/*.json, build/server/results.json, build/hil/results.json', allowEmpty: true
                    stash name: 'firmware', includes: 'build/firmware/default/**', allowEmpty: true
                }
            }
        }
        stage('L4 hardware-in-the-loop') {
            when {
                beforeAgent true
                anyOf { triggeredBy 'TimerTrigger'; expression { params.RUN_HIL }; buildingTag() }
            }
            agent { label 'sentinel-hil' }
            options { lock(resource: 'sentinel-node-bench') }
            steps {
                unstash 'firmware'
                unstash 'results'
                withCredentials([string(credentialsId: 'sentinel-api-key', variable: 'AMB82_API_KEY')]) {
                    sh 'ci/hil/deploy_edge_server.sh'
                    sh "ci/hil/flash_node.sh build/firmware/default/build ${params.HIL_SERIAL_PORT}"
                    sh """python3 -m pytest tests/hil --target hardware -p no:cacheprovider --timeout 1800 \
                          --api-url http://127.0.0.1:8080/api/v1 --node-ip ${params.HIL_NODE_IP} \
                          --fw-version \$(grep -oP 'FW_VERSION\\s+"\\K[^"]+' firmware/sentinel_node/sentinel_node.ino) \
                          --serial-port ${params.HIL_SERIAL_PORT} --soak-minutes ${params.SOAK_MINUTES} \
                          ${params.HIL_DESTRUCTIVE ? '--destructive' : ''} \
                          --junitxml build/hil_hw/junit.xml --req-json build/hil_hw/results.json"""
                }
                sh 'python3 ci/trace_report.py'
            }
            post {
                always {
                    junit allowEmptyResults: true, testResults: 'build/hil_hw/junit.xml'
                    archiveArtifacts allowEmptyArchive: true, artifacts: 'build/trace/**, build/hil_hw/**'
                    publishHTML(target: [reportName: 'Traceability (with hardware)', reportDir: 'build/trace',
                                         reportFiles: 'traceability.html', keepAll: true, allowMissing: true])
                }
            }
        }
    }
}

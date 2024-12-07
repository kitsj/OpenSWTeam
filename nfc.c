// NFC 감지 스레드
void* nfc_task(void* arg) {
    pid_t pid;
    int status;
    char* argv[] = { "nfc-poll", NULL };
    int fd = *(int*)arg;

    while (1) {
        pthread_mutex_lock(&flag_mutex);
        if (nfc_flag == 0) {
            pthread_mutex_unlock(&flag_mutex);
            printf("NFC 감지 중...\n");
            if (posix_spawn(&pid, "/bin/nfc-poll", NULL, NULL, argv, environ) == 0) {
                if (waitpid(pid, &status, 0) >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    pthread_mutex_lock(&flag_mutex);
                    nfc_flag = 1; // NFC 인증 성공
                    pthread_mutex_unlock(&flag_mutex);
                    printf("NFC 인증 성공\n");

                    time_t now = time(NULL); // 현재 시간 확인

                    // 복용 간격 및 복용 횟수 확인
                    pthread_mutex_lock(&flag_mutex);
                    if (m_count >= MAX_COUNT) {
                        printf("하루 약 복용 횟수를 초과했습니다. 부저 울림\n");
                        music(18); // 복용 제한 알림음
                    } else if (difftime(now, last_dose_time) < INTERVAL_TIME) {
                        int remaining = INTERVAL_TIME - (int)difftime(now, last_dose_time);
                        printf("복용 간격 충족되지 않음: %d초 남음. 부저 울림\n", remaining);
                        music(18); // 간격 제한 알림음
                    } else {
                        pthread_mutex_unlock(&flag_mutex);

                        // NFC 인증 성공 후 블루투스 입력 호출
                        if (bluetooth_input(fd)) {
                            printf("조건 충족: 모터 작동 시작\n");
                            one_two_Phase_Rotate_Angle(45, 1); // 스텝모터 45도 회전

                            pthread_mutex_lock(&flag_mutex);
                            m_count++;               // 복용 횟수 증가
                            last_dose_time = now;    // 마지막 복용 시간 갱신
                            printf("약 복용 횟수 %d\n", m_count);
                            pthread_mutex_unlock(&flag_mutex);
                        }
                    }
                    pthread_mutex_lock(&flag_mutex);
                    nfc_flag = 0; // NFC 플래그 초기화 (다시 감지 가능)
                    pthread_mutex_unlock(&flag_mutex);
                }
            } else {
                perror("nfc-poll 실행 실패");
            }
        } else {
            pthread_mutex_unlock(&flag_mutex);
        }
        sleep(1); // NFC 감지 주기
    }
    return NULL;
}

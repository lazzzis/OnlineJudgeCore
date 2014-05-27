#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/reg.h>
#include <sys/user.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/ptrace.h>

#include "core.h"
#include "logger.h"

extern int errno;

static
bool has_suffix(const std::string &str, const std::string &suffix) {
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static
void output_result() {
    //printf("%d %d %d\n", result, time_usage, memory_usage);
    FILE* result_file = fopen(PROBLEM::result_file.c_str(), "w");
    switch (PROBLEM::result){
        case 1:PROBLEM::status = "Compile Error";break;
        case 2:PROBLEM::status = "Time Limit Exceeded";break;
        case 3:PROBLEM::status = "Memory Limit Exceeded";break;
        case 4:PROBLEM::status = "Output Limit Exceeded";break;
        case 5:PROBLEM::status = "Runtime Error";break;
        case 6:PROBLEM::status = "Wrong Answer";break;
        case 7:PROBLEM::status = "Accepted";break;
        case 8:PROBLEM::status = "Presentation Error";break;
        default:PROBLEM::status = "System Error";break;
    }
    fprintf(result_file, "%s\n", PROBLEM::status.c_str());
    fprintf(result_file, "%d\n", PROBLEM::time_usage);
    fprintf(result_file, "%d\n", PROBLEM::memory_usage);
    fprintf(result_file, "%s\n", PROBLEM::extra_message.c_str());

    FM_LOG_TRACE("The final result is %s %d %d %s",
            PROBLEM::status.c_str(), PROBLEM::time_usage,
            PROBLEM::memory_usage, PROBLEM::extra_message.c_str());
}

static
void parse_arguments(int argc, char* argv[]) {
    int opt;
    extern char *optarg;

    while ((opt = getopt(argc, argv, "c:t:m:d:S:s")) != -1) {
        switch (opt) {
            case 'c': PROBLEM::code_path    = optarg;         break;
            case 't': PROBLEM::time_limit   = atoi(optarg);   break;
            case 'm': PROBLEM::memory_limit = atoi(optarg);   break;
            case 's': PROBLEM::spj          = true;           break;
            case 'S': PROBLEM::spj_lang     = atoi(optarg);   break;
            case 'd': PROBLEM::run_dir      = optarg;         break;
            default:
                //printf("unkown option\n");
                FM_LOG_WARNING("Unknown option provided: -%c %s", opt, optarg);
                exit(JUDGE_CONF::EXIT_BAD_PARAM);
        }
    }

    if (has_suffix(PROBLEM::code_path, ".cpp")) {
        PROBLEM::lang = JUDGE_CONF::LANG_CPP;
    } else if (has_suffix(PROBLEM::code_path, ".c")) {
        PROBLEM::lang = JUDGE_CONF::LANG_C;
    } else if (has_suffix(PROBLEM::code_path, ".java")) {
        PROBLEM::lang = JUDGE_CONF::LANG_JAVA;
    } else {
        //printf("unkwon language\n");
        FM_LOG_WARNING("It seems that you give me a language which I do not known now: %d", PROBLEM::lang);
        exit(JUDGE_CONF::EXIT_BAD_PARAM);
    }

    PROBLEM::exec_file = PROBLEM::run_dir + "/a.out";
    PROBLEM::input_file = PROBLEM::run_dir + "/in.in";
    PROBLEM::output_file = PROBLEM::run_dir + "/out.out";
    PROBLEM::exec_output = PROBLEM::run_dir + "/out.txt";
    PROBLEM::result_file = PROBLEM::run_dir + "/result.txt";
    PROBLEM::stdout_file_compiler = PROBLEM::run_dir + "/stdout_file_compiler.txt";
    PROBLEM::stderr_file_compiler = PROBLEM::run_dir + "/stderr_file_compiler.txt";

    if (PROBLEM::lang == JUDGE_CONF::LANG_JAVA) {
        PROBLEM::exec_file = PROBLEM::run_dir + "/Main";
        PROBLEM::time_limit *= JUDGE_CONF::JAVA_TIME_FACTOR;
        PROBLEM::memory_limit *= JUDGE_CONF::JAVA_MEM_FACTOR;
    }

    if (PROBLEM::spj) {
        switch (PROBLEM::spj_lang) {
            case 1:
            case 2: PROBLEM::spj_exec_file = PROBLEM::run_dir + "/SpecialJudge";break;
            case 3: PROBLEM::spj_exec_file = PROBLEM::run_dir + "/SpecialJudge";break;
            default:
                //printf("Unknown special judge language\n");
                FM_LOG_WARNING("OMG, I really do not kwon the special judge problem language.");
                exit(JUDGE_CONF::EXIT_BAD_PARAM);
        }

        PROBLEM::spj_output_file = PROBLEM::run_dir + "/spj_output.txt";
    }
}

static
void get_compile_error_message() {
    FILE *ce_msg = fopen(PROBLEM::stderr_file_compiler.c_str(), "r");
    std::string message = "";
    char tmp[1024];
    while (fgets(tmp, sizeof(tmp), ce_msg)) {
        message += tmp;
    }

    PROBLEM::extra_message = message;
}

static
void timeout(int signo) {
    if (signo == SIGALRM) {
        exit(JUDGE_CONF::EXIT_TIMEOUT);
    }
}

static
int malarm(int which, int milliseconds) {
    struct itimerval t;
    //设置时间限制
    t.it_value.tv_sec     = milliseconds / 1000;
    t.it_value.tv_usec    = milliseconds % 1000 * 1000; //微秒
    t.it_interval.tv_sec  = 0;
    t.it_interval.tv_usec = 0;
    return setitimer(which, &t, NULL);
}

static
void io_redirect() {
    FM_LOG_TRACE("Start to redirect the IO.");
    stdin = freopen(PROBLEM::input_file.c_str(), "r", stdin);
    stdout = freopen(PROBLEM::exec_output.c_str(), "w", stdout);
    //stderr = freopen("/dev/null", "w", stderr);

    if (stdin == NULL || stdout == NULL || stderr == NULL) {
        //printf("freopen error\n");
        FM_LOG_WARNING("It occur a error when freopen: stdin(%p) stdout(%p)", stdin, stdout);
        exit(JUDGE_CONF::EXIT_PRE_JUDGE);
    }
    FM_LOG_TRACE("redirect io is OK.");
}

static
void security_control() {
    struct passwd *nobody = getpwnam("nobody");
    if (nobody == NULL){
        //printf("no user name nobody\n");
        FM_LOG_WARNING("Well, where is nobody? I cannot live without him. %d: %s", errno, strerror(errno));
        exit(JUDGE_CONF::EXIT_SET_SECURITY);
    }

    //chdir
    if (EXIT_SUCCESS != chdir(PROBLEM::run_dir.c_str())) {
        //printf("chdir failed\n");
        FM_LOG_WARNING("chdir(%s) failed, %d: %s", PROBLEM::run_dir.c_str(), errno, strerror(errno));
        exit(JUDGE_CONF::EXIT_SET_SECURITY);
    }

    char cwd[1024], *tmp = getcwd(cwd, 1024);
    if (tmp == NULL) {
        //printf("getcwd failed\n");
        FM_LOG_WARNING("Oh, where i am now? I cannot getcwd. %d: %s", errno, strerror(errno));
        exit(JUDGE_CONF::EXIT_SET_SECURITY);
    }

    //chroot
    if (PROBLEM::lang != JUDGE_CONF::LANG_JAVA) {
        if (EXIT_SUCCESS != chroot(cwd)) {
            //printf("chroot failed\n");
            FM_LOG_WARNING("chroot(%s) failed. %d: %s", cwd, errno, strerror(errno));
            exit(JUDGE_CONF::EXIT_SET_SECURITY);
        }
    }

    //setuid
    if (EXIT_SUCCESS != setuid(nobody->pw_uid)) {
        //printf("set uid failed\n %d: %s\n", errno, strerror(errno));
        FM_LOG_WARNING("setuid(%d) failed. %d: %s", nobody->pw_uid, errno, strerror(errno));
        exit(JUDGE_CONF::EXIT_SET_SECURITY);
    }
}

static
void security_control_spj() {
    struct passwd *nobody = getpwnam("nobody");
    if (nobody == NULL) {
        //printf("no user name nobody\n");
        FM_LOG_WARNING("Well, where is nobody? I cannot live without him. %d: %s", errno, strerror(errno));
        exit(JUDGE_CONF::EXIT_SET_SECURITY);
    }

    if (EXIT_SUCCESS != chdir(PROBLEM::run_dir.c_str())) {
        //printf("chdir failed\n");
        FM_LOG_WARNING("chdir(%s) failed, %d: %s", PROBLEM::run_dir.c_str(), errno, strerror(errno));
        exit(JUDGE_CONF::EXIT_SET_SECURITY);
    }

    char cwd[1024], *tmp = getcwd(cwd, 1024);
    if (tmp == NULL) {
        //printf("getcwd failed\n");
        FM_LOG_WARNING("Oh, where i am now? I cannot getcwd. %d: %s", errno, strerror(errno));
        exit(JUDGE_CONF::EXIT_SET_SECURITY);
    }

    if (PROBLEM::spj_lang != JUDGE_CONF::LANG_JAVA) {
        if (EXIT_SUCCESS != chroot(cwd)) {
            //printf("chroot failed\n");
            FM_LOG_WARNING("chroot(%s) failed. %d: %s", cwd, errno, strerror(errno));
            exit(JUDGE_CONF::EXIT_SET_SECURITY);
        }
    }

    if (EXIT_SUCCESS != setuid(nobody->pw_uid)) {
        //printf("set uid failed\n");
        FM_LOG_WARNING("setuid(%d) failed. %d: %s", nobody->pw_uid, errno, strerror(errno));
        exit(JUDGE_CONF::EXIT_SET_SECURITY);
    }
}

static
int set_limit() {
    rlimit lim;

    lim.rlim_max = (PROBLEM::time_limit - PROBLEM::time_usage + 999) / 1000 + 1;//硬限制
    lim.rlim_cur = lim.rlim_max; //软限制
    if (setrlimit(RLIMIT_CPU, &lim) < 0) {
        //printf("setrlimit for RLIMIT_CPU error\n");
        FM_LOG_WARNING("error setrlimit for RLIMIT_CPU");
        exit(JUDGE_CONF::EXIT_SET_LIMIT);
    }

    //内存不能在此做限制


    //堆栈空间限制
    getrlimit(RLIMIT_STACK, &lim);
    //printf

    int rlim = JUDGE_CONF::STACK_SIZE_LIMIT * JUDGE_CONF::KILO;
    if (lim.rlim_max <= rlim) {
        //printf("cannot set stack size\n");
        FM_LOG_WARNING("cannot set stack size to higher(%d <= %d)", lim.rlim_max, rlim);
    } else {
        lim.rlim_max = rlim;
        lim.rlim_cur = rlim;

        if (setrlimit(RLIMIT_STACK, &lim) < 0) {
            //printf("setrlimt RLIMIT_STACK failed\n");
            FM_LOG_WARNING("error setrlimit for RLIMIT_STACK");
            exit(JUDGE_CONF::EXIT_SET_LIMIT);
        }
    }

    log_close(); //关闭log，防止log造成OLE

    //输出文件大小限制
    lim.rlim_max = PROBLEM::output_limit * JUDGE_CONF::KILO;
    lim.rlim_cur = lim.rlim_max;
    if (setrlimit(RLIMIT_FSIZE, &lim) < 0) {
        perror("setrlimit RLIMIT_FSIZE failed\n");
        exit(JUDGE_CONF::EXIT_SET_LIMIT);
    }
}


#include "rf_table.h"
//系统调用在进和出的时候都会暂停, 把控制权交给judge
static bool in_syscall = true;
static
bool is_valid_syscall(int lang, int syscall_id, pid_t child, user_regs_struct regs) {
    in_syscall = !in_syscall;
    //FM_LOG_DEBUG("syscall: %d, %s, count: %d", syscall_id, in_syscall?"in":"out", RF_table[syscall_id]);
    if (RF_table[syscall_id] == 0)
    {
        //如果RF_table中对应的syscall_id可以被调用的次数为0, 则为RF
        long addr;
        if(syscall_id == SYS_open)
        {
#if __WORDSIZE == 32
            addr = regs.ebx;
#else
            addr = regs.rdi;
#endif
#define LONGSIZE sizeof(long)
            union u{ unsigned long val; char chars[LONGSIZE]; }data;
            unsigned long i = 0, j = 0, k = 0;
            char filename[300];
            while (true)
            {
                data.val = ptrace(PTRACE_PEEKDATA, child, addr + i,  NULL);
                i += LONGSIZE;
                for (j = 0; j < LONGSIZE && data.chars[j] > 0 && k < 256; j++)
                {
                    filename[k++] = data.chars[j];
                }
                if (j < LONGSIZE && data.chars[j] == 0)
                    break;
            }
            filename[k] = 0;
            //FM_LOG_TRACE("syscall open: filename: %s", filename);
            if (strstr(filename, "..") != NULL)
            {
                return false;
            }
            if (strstr(filename, "/proc/") == filename)
            {
                return true;
            }
            if (strstr(filename, "/dev/tty") == filename)
            {
                PROBLEM::result = JUDGE_CONF::RE;
                exit(JUDGE_CONF::EXIT_OK);
            }
        }
        return false;
    } else if (RF_table[syscall_id] > 0) {
        //如果RF_table中对应的syscall_id可被调用的次数>0
        //且是在退出syscall的时候, 那么次数减一
        if (in_syscall == false)
            RF_table[syscall_id]--;
    } else {
        //RF_table中syscall_id对应的指<0, 表示是不限制调用的
        ;
    }
    return true;
}

static
void compiler_source_code() {
    pid_t compiler = fork();
    int status = 0;
    if (compiler < 0) {
        //printf("fork compiler error\n");
        FM_LOG_WARNING("error fork compiler");
        exit(JUDGE_CONF::EXIT_COMPILE);
    } else if (compiler == 0) {
        //log
        log_add_info("compiler");
        stdout = freopen(PROBLEM::stdout_file_compiler.c_str(), "w", stdout);
        stderr = freopen(PROBLEM::stderr_file_compiler.c_str(), "w", stderr);
        if (stdout == NULL || stderr == NULL) {
            //printf("error freopen stdout, stderr\n");
            FM_LOG_WARNING("error to freopen in compiler: stdout(%p) stderr(%p)", stdout, stderr);
            exit(JUDGE_CONF::EXIT_COMPILE);
        }

        malarm(ITIMER_REAL, JUDGE_CONF::COMPILE_TIME_LIMIT);
        switch (PROBLEM::lang) {
            case JUDGE_CONF::LANG_C:
                //printf("compiler gcc\n");
                FM_LOG_TRACE("Start: gcc -o %s %s -static -w -lm -std=c99 -O2 -DONLINE_JUDGE",
                        PROBLEM::exec_file.c_str(), PROBLEM::code_path.c_str());
                execlp("gcc", "gcc", "-o", PROBLEM::exec_file.c_str(), PROBLEM::code_path.c_str(),
                        "-static", "-w", "-lm", "-std=c99", "-O2", "-DONLINE_JUDGE", NULL);
                break;
            case JUDGE_CONF::LANG_CPP:
                //printf("compiler g++\n");
                FM_LOG_TRACE("Start: g++ -o %s %s -static -w -lm -O2 -DONLINE_JUDGE",
                        PROBLEM::exec_file.c_str(), PROBLEM::code_path.c_str());
                execlp("g++", "g++", "-o", PROBLEM::exec_file.c_str(), PROBLEM::code_path.c_str(),
                        "-static", "-w", "-lm", "-O2", "-DONLINE_JUDGE", NULL);
                break;
            case JUDGE_CONF::LANG_JAVA:
                //printf("compiler java\n");
                FM_LOG_TRACE("Start:javac %s -d %s", PROBLEM::code_path.c_str(), PROBLEM::run_dir.c_str());
                execlp("javac", "javac", PROBLEM::code_path.c_str(), "-d", PROBLEM::run_dir.c_str(), NULL);
        }
        //printf("exec error");
        FM_LOG_WARNING("exec compiler error");
        exit(JUDGE_CONF::EXIT_COMPILE);
    } else {
        pid_t w = waitpid(compiler, &status, WUNTRACED);
        if (w == -1) {
            //printf("waitpid error\n");
            FM_LOG_WARNING("waitpid error");
            exit(JUDGE_CONF::EXIT_COMPILE);
        }

        //printf("compiler finished\n");
        FM_LOG_TRACE("compiler finished");
        if (WIFEXITED(status)) {
            if (EXIT_SUCCESS == WEXITSTATUS(status)) {
                //printf("compile succeeded.\n");
                FM_LOG_TRACE("compile succeeded.");
            } else if (JUDGE_CONF::GCC_COMPILE_ERROR == WEXITSTATUS(status)){
                //printf("compile error\n");
                FM_LOG_TRACE("compile error");
                PROBLEM::result = JUDGE_CONF::CE;
                get_compile_error_message();
                exit(JUDGE_CONF::EXIT_OK);
            } else {
                //printf("compiler unkown exit status %d\n", WEXITSTATUS(status));
                FM_LOG_WARNING("Unknown error occur when compiling the source code.Exit status %d", WEXITSTATUS(status));
                exit(JUDGE_CONF::EXIT_COMPILE);
            }
        } else {
            if (WIFSIGNALED(status)){
                if (SIGALRM == WTERMSIG(status)) {
                    //printf("compiler time out\n");
                    FM_LOG_WARNING("Compile time out");
                    PROBLEM::result = JUDGE_CONF::CE;
                    PROBLEM::extra_message = "Compile Out of Time Limit";
                    exit(JUDGE_CONF::EXIT_OK);
                } else {
                    //printf("unkown signal\n");
                    FM_LOG_WARNING("Unknown signal when compile the source code.");
                }
            } else if (WIFSTOPPED(status)){
                //printf("stopped by signal\n");
                FM_LOG_WARNING("The compile process stopped by signal");
            } else {
                //printf("unknown stop reason");
                FM_LOG_WARNING("I don't kwon why the compile process stopped");
            }
            exit(JUDGE_CONF::EXIT_COMPILE);
        }
    }
}

static
void judge() {
    struct rusage rused;
    pid_t executive = fork();
    if (executive < 0) {
        printf("fork for child failed\n");
        exit(JUDGE_CONF::EXIT_PRE_JUDGE);
    } else if (executive == 0) {
        //log
        FM_LOG_TRACE("Start Judging.");
        io_redirect();

        security_control();

        int real_time_limit = PROBLEM::time_limit;
        if (EXIT_SUCCESS != malarm(ITIMER_REAL, real_time_limit)) {
            //printf("malarm failed\n");
            exit(JUDGE_CONF::EXIT_PRE_JUDGE);
        }

        //log
        set_limit();

        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0) {
            exit(JUDGE_CONF::EXIT_PRE_JUDGE_PTRACE);
        }

        if (PROBLEM::lang != JUDGE_CONF::LANG_JAVA){
            execl("./a.out", "a.out", NULL);
        } else {
            execlp("java", "java", "Main", NULL);
        }

        exit(JUDGE_CONF::EXIT_PRE_JUDGE_EXECLP);
    } else {
        int status = 0;
        int syscall_id = 0;
        struct user_regs_struct regs;

        init_RF_table(PROBLEM::lang);

        while (true) {
            if (wait4(executive, &status, 0, &rused) < 0) {
                //printf("wait4 failed\n");
                FM_LOG_WARNING("wait4 failed.");
                exit(JUDGE_CONF::EXIT_JUDGE);
            }

            if (WIFEXITED(status)) {
                if (PROBLEM::lang != JUDGE_CONF::LANG_JAVA ||
                    WEXITSTATUS(status) == EXIT_SUCCESS) {
                    //printf("normal quit\n");
                    FM_LOG_TRACE("OK, normal quit. All is good.");

                    //PROBLEM::result = JUDGE_CONF::PROCEED;
                } else {
                    //printf("abnormal quit\n");
                    FM_LOG_WARNING("oh, some error occured.Abnormal quit.");
                    PROBLEM::result = JUDGE_CONF::RE;
                }
                break;
            }

            if (WIFSIGNALED(status) ||
                (WIFSTOPPED(status) && WSTOPSIG(status) != SIGTRAP)) {
                int signo = 0;
                if (WIFSIGNALED(status)) {
                    signo = WTERMSIG(status);
                    FM_LOG_WARNING("child signaled by %d : %s", signo, strsignal(signo));
                } else {
                    signo = WSTOPSIG(status);
                    FM_LOG_WARNING("child stop by %d : %s\n", signo, strsignal(signo));
                }

                switch (signo) {
                    //TLE
                    case SIGALRM:
                    case SIGXCPU:
                    case SIGVTALRM:
                    case SIGKILL:
                        FM_LOG_TRACE("Well, Time Limit Exeeded");
                        PROBLEM::result = JUDGE_CONF::TLE;
                        break;
                    case SIGXFSZ:
                        FM_LOG_TRACE("File Limit Exceeded");
                        PROBLEM::result = JUDGE_CONF::OLE;
                        break;
                    case SIGSEGV:
                    case SIGFPE:
                    case SIGBUS:
                    case SIGABRT:
                        PROBLEM::result = JUDGE_CONF::RE;
                        break;
                    default:
                        PROBLEM::result = JUDGE_CONF::RE;
                        break;
                }

                ptrace(PTRACE_KILL, executive, NULL, NULL);
                break;
            }

            //MLE
            PROBLEM::memory_usage = std::max((long int)PROBLEM::memory_usage,
                    rused.ru_minflt * (getpagesize() / JUDGE_CONF::KILO));

            if (PROBLEM::memory_usage > PROBLEM::memory_limit) {
                //printf("MLE\n");
                PROBLEM::result = JUDGE_CONF::MLE;
                FM_LOG_TRACE("Well, Memory Limit Exceeded.");
                ptrace(PTRACE_KILL, executive, NULL, NULL);
                break;
            }

            //GET REGS
            if (ptrace(PTRACE_GETREGS, executive, NULL, &regs) < 0) {
                FM_LOG_WARNING("ptrace PTRACE_GETREGS failed");
                exit(JUDGE_CONF::EXIT_JUDGE);
            }

#ifdef __i386__
            syscall_id = regs.orig_eax;
#else
            syscall_id = regs.orig_rax;
#endif
            //printf("The child made a system call %llu\n", regs.orig_rax);

            if (syscall_id > 0 &&
                !is_valid_syscall(PROBLEM::lang, syscall_id, executive, regs)) {
                FM_LOG_WARNING("restricted fuction %d\n", syscall_id);
                if (syscall_id == SYS_rt_sigprocmask){
                    //printf("glibc failed\n");
                    FM_LOG_WARNING("The glibc failed.");
                } else {
                    //FM_LOG_WARNING("%d\n", SYS_write);
                    FM_LOG_WARNING("restricted fuction table");
                }
                PROBLEM::result = JUDGE_CONF::RE;
                ptrace(PTRACE_KILL, executive, NULL, NULL);
                break;
            }

            if (ptrace(PTRACE_SYSCALL, executive, NULL, NULL) < 0) {
                //printf("ptrace PTRACE_SYSCALL failed\n");
                FM_LOG_WARNING("ptrace PTRACE_SYSCALL failed.");
                exit(JUDGE_CONF::EXIT_JUDGE);
            }
        }
    }

    PROBLEM::time_usage += (rused.ru_utime.tv_sec * 1000 +
                            rused.ru_utime.tv_usec / 1000);
    PROBLEM::time_usage += (rused.ru_stime.tv_sec * 1000 +
                            rused.ru_stime.tv_usec / 1000);

}

static
int compare_output(std::string file_std, std::string file_exec) {
    FILE *fp_std = fopen(file_std.c_str(), "r");
    if (fp_std == NULL) {
        //printf("open standard output failed.\n");
        FM_LOG_WARNING("Open standard output file failed.");
        exit(JUDGE_CONF::EXIT_COMPARE);
    }

    FILE *fp_exe = fopen(file_exec.c_str(), "r");
    if (fp_exe == NULL) {
        //printf("open executive output failed\n");
        FM_LOG_WARNING("Open executive output file failed.");
        exit(JUDGE_CONF::EXIT_COMPARE);
    }
    int a, b, Na = 0, Nb = 0;
    enum {
        AC = JUDGE_CONF::AC,
        PE = JUDGE_CONF::PE,
        WA = JUDGE_CONF::WA
    }status = AC;
    while (true) {
        a = fgetc(fp_std);
        b = fgetc(fp_exe);
        Na++, Nb++;

        //统一\r和\n之间的区别
        if (a == '\r') a = '\n';
        if (b == '\r') b = '\n';
#define is_space_char(a) ((a == ' ') || (a == '\t') || (a == '\n'))

        if (feof(fp_std) && feof(fp_exe)){
            //文件结束
            break;
        } else if (feof(fp_std) || feof(fp_exe)) {
            //如果只有一个文件结束
            //但是另一个文件的末尾是回车
            //那么也当做AC处理
            FILE *fp_tmp;
            if (feof(fp_std)) {
                if (!is_space_char(b)) {
                    //printf("WA\n");
                    FM_LOG_TRACE("Well, Wrong Answer.");
                    status = WA;
                    break;
                }
                fp_tmp = fp_exe;
            } else {
                if (!is_space_char(a)) {
                    //printf("WA\n");
                    FM_LOG_TRACE("Well, Wrong Answer.");
                    status = WA;
                    break;
                }
                fp_tmp = fp_std;
            }
            int c;
            while (c = fgetc(fp_tmp), c != EOF) {
                if (c == '\r') c = '\n';
                if (!is_space_char(c)) {
                    //printf("WA\n");
                    FM_LOG_TRACE("Well, Wrong Answer.");
                    break;
                }
            }
            break;
        }

        //如果两个字符不同
        if (a != b) {
            status = PE;
            //过滤空白字符
            if (is_space_char(a) && is_space_char(b)) {
                continue;
            }
            if (is_space_char(a)) {
                //a是空白字符，过滤，退回b以便下一轮循环
                ungetc(b, fp_exe);
                Nb--;
            } else if (is_space_char(b)) {
                ungetc(a, fp_std);
                Na--;
            } else {
                //printf("WA\n");
                FM_LOG_TRACE("Well, Wrong Answer.");
                status = WA;
                break;
            }
        }
    }
    fclose(fp_std);
    fclose(fp_exe);
    return status;
}

static
void run_spj() {
    printf("start spj\n");
    pid_t spj_pid = fork();
    int status = 0;
    if (spj_pid < 0) {
        //printf("fork for spj failed\n");
        FM_LOG_WARNING("fork for special judge failed.So sad.");
        exit(JUDGE_CONF::EXIT_COMPARE_SPJ);
    } else if (spj_pid == 0) {
        //printf("spj\n");
        FM_LOG_TRACE("Woo, I will start special judge!");
        stdin = freopen(PROBLEM::exec_output.c_str(), "r", stdin);
        stdout = freopen(PROBLEM::spj_output_file.c_str(), "w", stdout);
        if (stdin == NULL || stdout == NULL) {
            //printf("failed to open files in spj\n");
            FM_LOG_WARNING("redirect io in spj failed.");
            exit(JUDGE_CONF::EXIT_COMPARE_SPJ);
        }
        //SPJ时间限制
        if (EXIT_SUCCESS != malarm(ITIMER_REAL, JUDGE_CONF::SPJ_TIME_LIMIT)) {
            //printf("spj set time limit failed\n");
            FM_LOG_WARNING("Set time limit for spj failed.");
            exit(JUDGE_CONF::EXIT_COMPARE_SPJ);
        }

        security_control_spj();

        if (PROBLEM::spj_lang != JUDGE_CONF::LANG_JAVA) {
            execl("./SpecialJudge", "SpecialJudge", NULL);
        } else {
            execlp("java", "java", "SpecialJudge", NULL);
        }

        exit(JUDGE_CONF::EXIT_COMPARE_SPJ_FORK);
    } else {
        if (wait4(spj_pid, &status, 0, NULL) < 0) {
            //printf("wait4 failed\n");
            FM_LOG_WARNING("wait4 failed.");
            exit(JUDGE_CONF::EXIT_COMPARE_SPJ);
        }

        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) == EXIT_SUCCESS) {
                //printf("spj normal quit\n");
                FM_LOG_TRACE("Well, SpecialJudge program normally quit.All is good.");
                return ;
            } else {
                //printf("spj abnormal termination %d\n", WEXITSTATUS(status));
                FM_LOG_WARNING("I am sorry to tell you that the special judge program abnormally terminated. %d", WEXITSTATUS(status));
            }
        } else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM) {
            //printf("spj time out\n");
            FM_LOG_WARNING("Well, the special judge program consume too much time.");
        } else {
            //printf("unkown termination\n");
            FM_LOG_WARNING("Actually, I do not kwon why the special judge program dead.");
        }
    }
}

static
void get_spj_result() {
    FILE *spj_result = fopen(PROBLEM::spj_output_file.c_str(), "r");
    char tmp[10];
    fgets(tmp, sizeof(tmp), spj_result);
    if (!strcmp(tmp, "AC")) {
        PROBLEM::result = JUDGE_CONF::AC;
    } else {
        PROBLEM::result = JUDGE_CONF::WA;
    }
}

int main(int argc, char *argv[]) {

    log_open("./core_log.txt");

    atexit(output_result);

    if (geteuid() != 0) {
        //printf("must run as root\n");
        FM_LOG_FATAL("You must run this program as root.");
        exit(JUDGE_CONF::EXIT_UNPRIVILEGED);
    }

    parse_arguments(argc, argv);

    JUDGE_CONF::JUDGE_TIME_LIMIT += PROBLEM::time_limit;

    if (EXIT_SUCCESS != malarm(ITIMER_REAL, JUDGE_CONF::JUDGE_TIME_LIMIT)) {
        //printf("set alarm for judge failed, %d: %s\n", errno, strerror(errno));
        FM_LOG_WARNING("Set the alarm for this judge program failed, %d: %s", errno, strerror(errno));
        exit(JUDGE_CONF::EXIT_VERY_FIRST);
    }
    signal(SIGALRM, timeout);

    compiler_source_code();

    judge();

    if (PROBLEM::spj) {
        run_spj();
        get_spj_result();
    } else {
        if (PROBLEM::result == JUDGE_CONF::SE)
            PROBLEM::result = compare_output(PROBLEM::output_file, PROBLEM::exec_output);
    }

    //output_result();

    return 0;
}

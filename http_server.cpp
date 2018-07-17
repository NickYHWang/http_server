#include <queue>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <signal.h>
#include <sys/wait.h>
#include <pthread.h>
#include <string>
#include <iostream>
#include <sstream>
#include <sys/poll.h>
#include <linux/fs.h>
#include <sys/epoll.h>


using namespace std;
/**
* @brief 线程类
*/
class ThreadPool{
    typedef void* arg_t;			///< 任务参数类型
    typedef void (*func_t)(arg_t);	///< 人物函数类型
public:
    ThreadPool(size_t count):destroy(false){

        // 初始化互斥锁和条件变量
        pthread_mutex_init(&mutex,NULL);
        pthread_cond_init(&cond,NULL);

        // 初始化线程组
        for(int i=0;i<count;i++){
            pthread_t tid;
            pthread_create(&tid,NULL,reinterpret_cast<void*(*)(void*)>(&ThreadPool::Route),this);
            threads.push_back(tid);
        }        
    }
    ~ThreadPool(){
        // 通知线程退出
        destroy = true;
        pthread_cond_broadcast(&cond);
        for(vector<pthread_t>::iterator it = threads.begin();it != threads.end();it++){
            pthread_join(*it,NULL);
        }

        // 销毁互斥锁和条件变量
        pthread_mutex_destroy(&mutex);
        pthread_cond_destroy(&cond);
    }
    void AddJob(func_t func,arg_t arg){
        pthread_mutex_lock(&mutex);
        tasks.push(func);
        args.push(arg);
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&mutex);
    }
private:
    static void Route(ThreadPool* pPool){
        for(;;){
            pthread_mutex_lock(&(pPool->mutex));
            // 如果没有任务等待
            while(pPool->tasks.empty() && !pPool->destroy){
                pthread_cond_wait(&(pPool->cond),&(pPool->mutex));
            }

            // 线程退出
            if(pPool->destroy){
                pthread_mutex_unlock(&(pPool->mutex));
                break;
            }

            // 获取任务
            func_t task = pPool->tasks.front();
            pPool->tasks.pop();
            arg_t arg = pPool->args.front();
            pPool->args.pop();
            pthread_mutex_unlock(&(pPool->mutex));

            // 执行任务
            task(arg);
        }
    }
private:
    vector<pthread_t> threads;  ///< 线程组
    queue<func_t> tasks;        ///< 任务队列
    queue<arg_t> args;          ///< 参数队列
    pthread_mutex_t mutex;	 ///< 互斥锁
    pthread_cond_t cond;		 ///< 条件变量
    bool destroy;               ///< 线程池销毁标志
};

enum IOMultiType{
SELECT,
POLL,
EPOLL
};

class IOMuitiplier{
public:
	virtual void AddFd(int fd) = 0;
	virtual void DelFd(int fd) = 0;
	virtual vector<int> Wait() = 0;
};


class Poll:public IOMuitiplier{
public:
	void AddFd(int fd){}
	void DelFd(int fd){}
	virtual vector<int> Wait() {
		return vector<int>();
	}
private:
	struct pollfd poll_fd[INR_OPEN_MAX];
	int count;
};
class Select:public IOMuitiplier{
public:
	void AddFd(int fd){}
	void DelFd(int fd){}
	virtual vector<int> Wait() {
		return vector<int>();
	}
private:

};
class Epoll:public IOMuitiplier{
public:
	Epoll():fd_count(0){
		epoll_fd = epoll_create(INR_OPEN_MAX);
		
	}
	void AddFd(int fd){
			if(fd_count+1 == INR_OPEN_MAX) {
				fprintf(stderr,"connfd size over %d",INR_OPEN_MAX);
				close(fd);
			}
	    struct epoll_event evt;
        evt.data.fd = fd;
        evt.events = EPOLLIN;
        epoll_ctl(epoll_fd,EPOLL_CTL_ADD,fd,&evt);
	  fd_count++;
	}
	void DelFd(int fd){
	  struct epoll_event evt;
        evt.data.fd = fd;
        evt.events = EPOLLIN;
        epoll_ctl(epoll_fd,EPOLL_CTL_DEL,fd,&evt);
		fd_count--;
	}
	virtual vector<int> Wait() {
        struct epoll_event out_evts[fd_count];
        int fd_cnt = epoll_wait(epoll_fd,out_evts,fd_count,-1);
		vector<int> res;
		for(int i=0;i<fd_cnt;i++){
            if(out_evts[i].events & EPOLLIN){
				res.push_back(out_evts[i].data.fd);
			}
		}
		return res;
	}
private:
	int epoll_fd;
	int fd_count;
};
class IOMultiplierFactory{
public:
	static IOMuitiplier* Create(IOMultiType type){
		switch(type){
		case SELECT:return new Select;
		case POLL:return new Poll;
		case EPOLL:return new Epoll;
		default:
			// TODO:
				return NULL;
		}
	}
};
void show_info(int connfd)
{
     // 获取本地连接信息
    struct sockaddr_in local_addr;
    bzero(&local_addr,sizeof(local_addr));
    socklen_t local_addr_len = sizeof(local_addr);
    getsockname(connfd,(struct sockaddr*)&local_addr,&local_addr_len);
    printf("server local %s:%d\n",inet_ntoa(local_addr.sin_addr),ntohs(local_addr.sin_port));

     // 获取远程连接信息
    struct sockaddr_in peer_addr;
    bzero(&peer_addr,sizeof(peer_addr));
    socklen_t peer_addr_len = sizeof(peer_addr);
    getpeername(connfd,(struct sockaddr*)&peer_addr,&peer_addr_len);
    printf("server peer %s:%d\n",inet_ntoa(peer_addr.sin_addr),ntohs(peer_addr.sin_port));
}
class Server{
	struct SelectInfo{
		Server* server;
		int fd;
		SelectInfo(Server* server,int fd):server(server),fd(fd){}
	};
public:
	Server(string const&ip,int port,string const& workDir,ThreadPool& pool)
	:ip_(ip),port_(port),workDir_(workDir),pool_(pool){
		Init();
		Bind();
		Listen();
	}
	void Start(IOMultiType type){
		iohander = IOMultiplierFactory::Create(type);
		iohander->AddFd(listenFd);
		while(true){
			fdvec = iohander->Wait();
			for(int i = 0;i<fdvec.size();i++){
				int fd = fdvec[i];
				if(listenFd == fd){
					// 创建连接套节字
					CreateFd();
				}else{
					SelectInfo* info = new SelectInfo(this,fd);
					pool_.AddJob(&Server::OnHandle,info);
				}
			}
		}
	}
private:
	int Init(){
	    listenFd = socket(AF_INET,SOCK_STREAM,0);
	    if(-1 == listenFd) {
		perror("listenfd open err");
		return 1;
	    }
	    printf("socket create OK\n");

	    int flag = 1;
	    setsockopt(listenFd,SOL_SOCKET,SO_REUSEADDR,&flag,sizeof(flag));

	    struct sockaddr_in local_addr;
	    bzero(&local_addr,sizeof(local_addr));
	    local_addr.sin_family = AF_INET;
	    local_addr.sin_addr.s_addr = inet_addr(ip_.c_str());
	    local_addr.sin_port = htons(port_);

	    if(-1 == bind(listenFd,(struct sockaddr*)&local_addr,sizeof(local_addr))) {
		perror("bind err");
		return 1;
	    }
	    printf("bind OK\n");

	    if(-1 == listen(listenFd,10)) {
		perror("listen err");
		return 1;
	    }
	    printf("listen OK\n");
	}
	void Bind(){}
	void Listen(){}
	static void OnHandle(void* arg){
	    SelectInfo* info = static_cast<SelectInfo*>(arg);
	    int fd = info->fd;
	    char buf[BUFSIZ];
		bzero(buf,BUFSIZ);
		ssize_t len;
		if((len = read(fd,buf,BUFSIZ-1)) == -1) {
		    perror("read err");
		    pthread_exit((void*)1);
		  }
		if(0 == len) {

		    printf("close %d\n",fd);
		    close(fd);
		    info->server->DeleteFd(fd);
		    return;
		  }
		printf("server recv:%s\n",buf);

		  // 报文解析，获取URI
		int url_start = 0;
		int url_end = 0;
		for(int j=0; j<len; j++) {
		    if(buf[j] == ' ') {
		        if(0 == url_start) {
		            url_start = j+1;
		        } else {
		            url_end = j;
		            break;
		        }

		    }
		}
		string url(buf+url_start,url_end-url_start);
		cout << "url:" << url << endl;
		string file = info->server->workDir_ + url;
		cout << "file" << file << endl;

		char* accept = strstr(buf,"Accept:");
		if(NULL == accept) {
		    perror("No Accept");
		}
		char* type = accept + sizeof("Accept:");
		int typeLen;
		for(int j=0; j< len - (type - buf); j++) {
		    if(*(type+j) == ',' || *(type+j) == '\n') {
		        typeLen = j;
		        break;
		    }
		}

		int filefd = open(file.c_str(),O_RDONLY);
		if(-1 == filefd) {
		    perror("open file err");
		    // exit(1);
		}
		ostringstream oss;
		oss << "HTTP/1.1 200 OK\n";

		struct stat file_stat;
		fstat(filefd,&file_stat);
		oss << "Content-Length:" << file_stat.st_size << endl;
		oss << "Content-Type:" << string(type,typeLen) << endl << endl;
		string status = oss.str();
		write(fd,status.c_str(),status.size());
		if(-1 == sendfile(fd,filefd,NULL,file_stat.st_size)) {
		    perror("sendfile err");
		    // exit(1);
		}
		close(filefd);
	   	delete info;
	}
	void DeleteFd(int fd){
		iohander->DelFd(fd);
	}
	void CreateFd(){
                printf("accept listenfd\n");
                int connfd = accept(listenFd,NULL,NULL);
                if(-1 == connfd) {
                    perror("accept err");
                } else {
		            iohander->AddFd(connfd);
                }
	}
private:
	string const ip_;
	int port_;
	int listenFd;/// 监听套节字
	string const workDir_;
	ThreadPool& pool_;
	IOMuitiplier* iohander;
	vector<int> fdvec;
};


int main(int argc,char* argv[])
{
	int nthread = 6;
	string ip = "0.0.0.0";
	int port = 8000;
	string workDir = argv[1];
	IOMultiType type = EPOLL;
    //if(4 != argc) {
     //   printf("usage:%s -j [jobs] -m [IO Multi Type]  -i [ip] -p [port] <dir>\n",argv[0]);
     //   return 1;
     //}

    ThreadPool pool(nthread);
    Server server(ip,port,workDir,pool);
    server.Start(type);
}

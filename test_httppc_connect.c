/****************************************************************************************
 * 编写日期：2008年12月                                                                 *
 ****************************************************************************************/

#include "app/MyUtil.h"

#define LOG CMyUtil::showLog

#ifdef WIN32
//#include <winsock2.h>
#include <stdio.h>
#endif
#ifdef LINUX
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#endif
#include "wei_util.h"
#include "httppc_ntlm.h"
#include "test_httppc_connect.h"

struct s_wei_http_response{
	char    version[10];
	int     code;
	int     header_num;
	char ** header;
	char *  body;
	bool    is_suppoort_ntlm;
	char *  proxy_connection;
	char *  proxy_authenticate;
};

static char * httppc_proxy_ip = NULL,* httppc_host = NULL,* httppc_domain = NULL, 
            * httppc_user = NULL, * httppc_passwd = NULL;
static UINT httppc_proxy_port;

static int httppc_sock = -1;
static bool is_httppc_connected = false;

/******************************** PART 1 :HTTP util 工具集 *******************************/
PRIVATE char * wei_get_domain_ip(IN char * ip_str){
	struct hostent * p;
	
	p = gethostbyname(ip_str);
	if(p == NULL)
		return NULL;
	return inet_ntoa(* (struct in_addr *)(* p->h_addr_list));
}

PRIVATE bool wei_http_analyse_response_startline(OUT s_wei_http_response * response,IN OUT char * startline){
	char * part[3];
	memset(part,0,sizeof(part));
	wei_util_replace_all(startline,"  "," ");
	if(!wei_util_split(startline," ",3,part))
		return false;
	char * ch = strchr(part[0],'/');
	if(ch == NULL)
		return false;
	strcpy(response->version ,ch + 1);

	if(!wei_util_check_allnum(part[1]))
		return false;
	response->code = atoi(part[1]);
	return true;
}

PRIVATE char * wei_http_analyse_get_value(IN s_wei_http_response * response, IN const char * name,IN int index){
	int num = 0;
	char * ch,name_1[128],name_2[128];
	if(strlen(name) > 126)
		return NULL;
	sprintf(name_1,"%s:",name);
	sprintf(name_2,"%s :",name);
	for(int i = 1; i < response->header_num; i ++){
		if(wei_util_str_compare(response->header[i],name_1,false,strlen(name_1))){
			if(index <= num){
				ch = response->header[i] + strlen(name_1);
				while(* ch == ' ')
					ch ++;
				return ch;
			}else{
				num ++;
			}
		}else if(wei_util_str_compare(response->header[i],name_2,false,strlen(name_2))){
			if(index <= num){
				ch = response->header[i] + strlen(name_2);
				while(* ch == ' ')
					ch ++;
				return ch;
			}else{
				num ++;
			}
		}

	}
	return NULL;
}

PRIVATE bool wei_http_parse_reponse(IN char * buf,IN int buf_len,OUT s_wei_http_response * http_response){
	char * level[2]; // split to http header and http body

	memset(http_response, 0 ,sizeof(s_wei_http_response));
	
	//分开header ＆ body
	if(!wei_util_split(buf,"\r\n\r\n", 2 ,level)){
		return false;
	}
	http_response->body = level[1];
	http_response->header_num = wei_util_get_str_num(level[0],"\r\n") + 1;
	if(http_response->header_num < 2)
		return false;
	http_response->header = (char **) malloc( sizeof(char *) * http_response->header_num );
	memset(http_response->header,0,sizeof(http_response->header));


	if(!wei_util_split(level[0],"\r\n",http_response->header_num,http_response->header)){
		free(http_response->header);
		return false;
	}

	for(int i = 0 ; i < http_response->header_num; i++){
		wei_util_str_trim(http_response->header[i]);
		wei_util_replace_all(http_response->header[i],"  "," ");
	}
	//分析消息头
	if(!wei_http_analyse_response_startline(http_response,http_response->header[0]))
		return false;

	switch(http_response->code){
	case 407:
	{
		int index = 0;
		char * value = NULL;
		while((value = wei_http_analyse_get_value(http_response,"Proxy-Authenticate",index)) != NULL){
			if(wei_util_str_compare_trim(value,"NTLM",false)){
				http_response->is_suppoort_ntlm = true;
				break;
			}
			index ++;
		}
		
		http_response->proxy_connection = wei_http_analyse_get_value(http_response,"Proxy-Connection",0);
		http_response->proxy_authenticate = wei_http_analyse_get_value(http_response,"Proxy-Authenticate",0);
		break;
	}
	default:
		break;
	}
	free(http_response->header);
	return true;
}
// End of PART 1 :HTTP util 工具集

/******************************** PART 2: proxy CONNECT 建立 *******************************/

/******************* PART 2.1 HTTP通信的通用函数 ********************/
PRIVATE static struct sockaddr_in httppc_proxyaddr;

/** 创建TCP的连接，返回socket*/
PRIVATE int wei_httppc_create_connect(){
	int sock;
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if(sock < 0)
		return -1;
	if(connect(sock,(struct sockaddr *)& httppc_proxyaddr,sizeof(struct sockaddr_in)) != 0){
#ifdef WIN32
		closesocket(sock);
#else
		close(sock);
#endif
		return -2;
	}
	return sock;
}

/** 关闭TCP连接 */
PRIVATE void wei_httppc_close_connect(int * sock){
	if(* sock <= 0)
		return;
#ifdef WIN32
		closesocket(* sock);
#else
		close(* sock);
#endif
		* sock = -1;
		LOG(LOG_DEBUG,"--------- close connect ------------------");
}

//********* End of PART 2.1 HTTP通信的通用函数

/** 设置HTTP Proxy Client的相关参数*/
bool wei_set_proxy_info(IN char * the_proxy_ip,IN UINT the_proxy_port,IN char * the_host,
							   IN char * the_domain,IN char * the_user, IN char * the_passwd){
	if(the_proxy_ip == NULL && strlen(the_proxy_ip))
		return false;
	if(the_proxy_port <= 0 || the_proxy_port > 65535)
		return false;
	LOG(LOG_DEBUG,"proxy = %s " , the_proxy_ip);
	if(httppc_proxy_ip != NULL)
		free(httppc_proxy_ip);
	httppc_proxy_ip = (char *) malloc(strlen(the_proxy_ip) + 1);

 	char * p = wei_get_domain_ip(the_proxy_ip);
	if( p == NULL)
		return false;
	strcpy(httppc_proxy_ip,p);

	if(httppc_host != NULL)
		free(httppc_host);
	if(the_host == NULL){
		httppc_host = NULL;
	}else{
		httppc_host = (char *) malloc(strlen(the_host));
		strcpy(httppc_host,the_host);
	}

	if(httppc_domain != NULL)
		free(httppc_domain);
	if(the_domain == NULL){
		httppc_domain = NULL;
	}else{
		httppc_domain = (char *) malloc(strlen(the_domain));
		strcpy(httppc_domain,the_domain);
	}

	if(httppc_user != NULL)
		free(httppc_user);
	if(the_user == NULL){
		httppc_user = NULL;
	}else{
		httppc_user = (char *) malloc(strlen(the_user));
		strcpy(httppc_user,the_user);
	}

	if(httppc_passwd != NULL)
		free(httppc_passwd);
	if(the_passwd == NULL){
		httppc_passwd = NULL;
	}else{
		httppc_passwd = (char *) malloc(strlen(the_passwd));
		strcpy(httppc_passwd,the_passwd);
	}

	httppc_proxy_port = the_proxy_port;

	memset(&httppc_proxyaddr,0,sizeof(struct sockaddr_in));
	httppc_proxyaddr.sin_family=AF_INET;
	httppc_proxyaddr.sin_port=htons(httppc_proxy_port);
	httppc_proxyaddr.sin_addr.s_addr = inet_addr(httppc_proxy_ip);//只能用于x.x.x.x的格式

	LOG(LOG_DEBUG,"proxy = %s " , httppc_proxy_ip);
	return true;
}

/** 清除HTTP Proxy Client的相关参数*/
void wei_reset_proxy_info(){
	if(httppc_proxy_ip != NULL)
		free(httppc_proxy_ip);
	httppc_proxy_ip = NULL;

	if(httppc_host != NULL)
		free(httppc_host);
	httppc_host = NULL;

	if(httppc_domain != NULL)
		free(httppc_domain);
	httppc_domain = NULL;

	if(httppc_user != NULL)
		free(httppc_user);
	httppc_user = NULL;

	if(httppc_passwd != NULL)
		free(httppc_passwd);
	httppc_passwd = NULL;

	httppc_proxy_port = 0;
	memset(&httppc_proxyaddr,0,sizeof(struct sockaddr_in));
}


PUBLIC bool wei_is_httppc_connected(){
	return is_httppc_connected;
}

/** main：可以在main中初始化PROXY参数后，调用此连接。我们的例子使用了HTTP CONNECT的方式。*/
int wei_httppc_connect(char * remote_ip ,int remote_port){
	struct sockaddr_in servaddr;
	int result = 0,recv_len = 0;
	char packet_buff[2048];
	s_wei_http_response response;

	memset(packet_buff,0,2048);
	memset(&servaddr,0,sizeof(servaddr));
	servaddr.sin_family=AF_INET;
	servaddr.sin_port=htons(httppc_proxy_port);
	servaddr.sin_addr.s_addr = inet_addr(httppc_proxy_ip);//只能用于x.x.x.x的格式

	if((httppc_sock = wei_httppc_create_connect()) <= 0)
		return httppc_sock;

	sprintf(packet_buff,"CONNECT %s:%d HTTP/1.1 \r\n" 
	        "User-Agent: My Service Enpoint 1.0\r\n"
	        "Host: %s:%d\r\n"
	        "Proxy-Connection: Keep-Alive\r\n"
	        "Pragma: no-cache\r\n"
		    "Content-Length: 0\r\n"
	        "\r\n", remote_ip,remote_port,remote_ip,remote_port);
	LOG(LOG_NET,"发送:\n%s",packet_buff);
	if(send(httppc_sock,packet_buff,strlen(packet_buff),0) <= 0){
		wei_httppc_close_connect(&httppc_sock);
		return -3;
	}

	memset(packet_buff,0,2048);
	if((recv_len = recv(httppc_sock,packet_buff,2048,0)) <= 0){
		wei_httppc_close_connect(&httppc_sock);
		return -4;
	}
	
	LOG(LOG_NET,"recv packet len = %d\n%s",recv_len,packet_buff);

	if(!wei_http_parse_reponse(packet_buff,recv_len,&response)){
		wei_httppc_close_connect(&httppc_sock);
		return -1;
	}
	switch(response.code){
	case 200:
		return httppc_sock;
	case 407:
		if(!response.is_suppoort_ntlm){
			wei_httppc_close_connect(&httppc_sock);
			return -1;
		}
		if(wei_util_str_compare_trim(response.proxy_connection,"close",false)){
			LOG(LOG_NET,"----------connect close------------");
			wei_httppc_close_connect(&httppc_sock);
			//创建信的连接
			if((httppc_sock = wei_httppc_create_connect()) <= 0)
				return httppc_sock;
		}
		//发送 type
		break;
	default:
		wei_httppc_close_connect(&httppc_sock);
		return NULL;
	}
	
	// NTLM type1 flow:
	char buf[1024];
	memset(buf,0,1024);
	wei_ntlm_make_type1_base64(true,httppc_domain,httppc_host,AUTHOR_NTLM_SESSION,buf);
	sprintf(packet_buff,"CONNECT %s:%d HTTP/1.1 \r\n" 
	        "User-Agent: My Service Enpoint 1.0\r\n"
	        "Host: %s:%d\r\n"
	        "Proxy-Connection: Keep-Alive\r\n"
	        "Pragma: no-cache\r\n"
			"Proxy-Authorization: Negotiate %s\r\n"
			"Content-Length: 0\r\n"
	        "\r\n", remote_ip,remote_port,remote_ip,remote_port,buf);
	LOG(LOG_NET,"发送:\n%s",packet_buff);
	if(send(httppc_sock,packet_buff,strlen(packet_buff),0) <= 0){
		wei_httppc_close_connect(&httppc_sock);
		return -3;
	}

	memset(packet_buff,0,2048);
	if((recv_len = recv(httppc_sock,packet_buff,2048,0)) <= 0){
		wei_httppc_close_connect(&httppc_sock);
		return -4;
	}
	LOG(LOG_DEBUG,"recv packet len = %d\n%s",recv_len,packet_buff);

	if(!wei_http_parse_reponse(packet_buff,recv_len,&response)){
		wei_httppc_close_connect(&httppc_sock);
		return -1;
	}
	if(response.proxy_authenticate == NULL)
		return -1;
	char * type2 = strchr(response.proxy_authenticate,' ');
	if(type2 == NULL)
		type2 = response.proxy_authenticate;
	while(* type2 == ' ' )
		type2 ++;

	T_NTLM_TYPE_2_MSG  type2_msg;
	if(!wei_ntlm_decode_type2(true,type2,&type2_msg)){
		wei_httppc_close_connect(&httppc_sock);
		return -1;
	}
	LOG(LOG_DEBUG,"---------ok,get type2 message! ------------------");

	if(!wei_ntlm_make_type3_base64(true,httppc_domain,httppc_host,httppc_user,httppc_passwd,
	 &type2_msg,buf)){
		LOG(LOG_DEBUG,"ERROR,make type3 message!");
		wei_httppc_close_connect(&httppc_sock);
		if(type2_msg.target_info != NULL)
			free(type2_msg.target_info);
		return -1;
	}
	if(type2_msg.target_info != NULL)
		free(type2_msg.target_info);
	sprintf(packet_buff,"CONNECT %s:%d HTTP/1.1 \r\n" 
	        "User-Agent: My Service Enpoint 1.0\r\n"
	        "Host: %s:%d\r\n"
	        "Proxy-Connection: Keep-Alive\r\n"
	        "Pragma: no-cache\r\n"
			"Proxy-Authorization: Negotiate %s\r\n"
			"Content-Length: 0\r\n"
	        "\r\n", remote_ip,remote_port,remote_ip,remote_port,buf);
	LOG(LOG_NET,"发送:\n%s",packet_buff);
	if(send(httppc_sock,packet_buff,strlen(packet_buff),0) <= 0){
		wei_httppc_close_connect(&httppc_sock);
		return -3;
	}

	memset(packet_buff,0,2048);
	if((recv_len = recv(httppc_sock,packet_buff,2048,0)) <= 0){
		wei_httppc_close_connect(&httppc_sock);
		return -4;
	}
	LOG(LOG_NET,"recv packet len = %d\n%s",recv_len,packet_buff);
	if(!wei_http_parse_reponse(packet_buff,recv_len,&response)){
		wei_httppc_close_connect(&httppc_sock);
		return -1;
	}

	if(response.code > 299 || response.code < 200){
		LOG(LOG_DEBUG,"ERROR, Do not connect the remote entity, code %d",response.code);
		wei_httppc_close_connect(&httppc_sock);
		return -1;
	}

	LOG(LOG_DEBUG,"response.code = %d",response.code);
	return httppc_sock;
}
//**** End of PART 2: proxy CONNECT 建立

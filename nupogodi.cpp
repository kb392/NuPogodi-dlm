#include <windows.h>
#include "rsl/dlmintf.h"
#include <string>
#include "mssup.h"
#include <map>
#include <amqp_tcp_socket.h>


char * rsGetStringParam(int iParam, char * defStr) {
    VALUE *vString;
    if (!GetParm (iParam,&vString) || vString->v_type != V_STRING) {
        if(defStr)
            return defStr;
        else
            RslError("Параметр №%i должен быть строкой",(iParam+1));
        }
    return vString->value.string;
}

char * rsGetFilePathParam(int iParam) {
    VALUE *vFilePath;
    if (!GetParm (iParam,&vFilePath) || vFilePath->v_type != V_STRING)
        RslError("Параметр №%i должен быть строкой",(iParam+1));
    char * sPath=(char *)malloc(sizeof(char)*strlen(vFilePath->value.string)+sizeof(char));
    OemToCharBuff(vFilePath->value.string, sPath, strlen(vFilePath->value.string));
    sPath[strlen(vFilePath->value.string)]='\0';
    return sPath;
}


/*
char * UTF8To866(const char * s) {
    BSTR    bstrWide;
    char *  sRet;
    int     nLength;

    nLength = MultiByteToWideChar(CP_UTF8, 0, s, strlen(s) + 1, NULL, NULL);
    bstrWide = SysAllocStringLen(NULL, nLength);

    MultiByteToWideChar(CP_UTF8, 0, s, strlen(s) + 1, bstrWide, nLength);

    nLength = WideCharToMultiByte(CP_ACP, 0, bstrWide, -1, NULL, 0, NULL, NULL);
    sRet    = (char *)malloc(nLength+1);
    sRet[nLength]='\0';

    WideCharToMultiByte(CP_ACP, 0, bstrWide, -1, sRet, nLength, NULL, NULL);
    SysFreeString(bstrWide);

    CharToOemBuff(sRet, sRet, nLength);

    return sRet;
}
*/

char * nupogodiGetFileContent(const char * sFilePath) {
    FILE *f = fopen(sFilePath, "rb");
    if (f==NULL) 
        RslError("Ошибка при открытии файла %s", sFilePath);

    fseek(f, 0, SEEK_END);
    size_t fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char * messagebody;
    if(messagebody = (char *)malloc(fsize + 1)) { 
        fread(messagebody, fsize, 1, f);
        messagebody[fsize] = 0;
    }
    fclose(f);

    return messagebody;
}

void nupogodiAmpqPropToRsVal(amqp_bytes_t rmqprop, VALUE * prsval) {
    char * p = (char *)iNewMem(rmqprop.len + 1);
    if (!p) 
        RslError("memory error");

    memcpy(p, rmqprop.bytes, rmqprop.len);
    p[rmqprop.len] = '\0';

    ValueClear(prsval);
    ValueSet(prsval, V_STRING, p);

    iDoneMem(p);
}

std::string nupogodiAmpqPropToString(amqp_bytes_t rmqprop) {
    char * p = (char *)iNewMem(rmqprop.len + 1);
    if (!p) 
        RslError("memory error");

    memcpy(p, rmqprop.bytes, rmqprop.len);
    p[rmqprop.len] = '\0';

    return std::string(p);
}


class  TNuPogodi {

    bool set_socket_error(amqp_rpc_reply_t x, char const *context) {
        switch (x.reply_type) {
            case AMQP_RESPONSE_NORMAL:
                return 1;

            case AMQP_RESPONSE_NONE:
                _snprintf_s(error_buffer,sizeof(error_buffer), _TRUNCATE, "AMPQ ERROR. %s missing RPC reply type", context);
                break;

            case AMQP_RESPONSE_LIBRARY_EXCEPTION:
                _snprintf_s(error_buffer,sizeof(error_buffer), _TRUNCATE, "AMPQ ERROR. %s: %s", context, amqp_error_string2(x.library_error));
                break;

            case AMQP_RESPONSE_SERVER_EXCEPTION:
                switch (x.reply.id) {

                    case AMQP_CONNECTION_CLOSE_METHOD: {
                        amqp_connection_close_t *m = (amqp_connection_close_t *)x.reply.decoded;

                        _snprintf_s(error_buffer,sizeof(error_buffer), _TRUNCATE, "AMPQ ERROR. %s: server connection error %uh, message: %.*s\n", 
                                    context, m->reply_code, (int)m->reply_text.len, (char *)m->reply_text.bytes);
                        break;
                        }

                    case AMQP_CHANNEL_CLOSE_METHOD: {
                        amqp_channel_close_t *m = (amqp_channel_close_t *)x.reply.decoded;
                        _snprintf_s(error_buffer,sizeof(error_buffer), _TRUNCATE, "AMPQ ERROR. %s: server channel error %uh, message: %.*s\n",
                                 context, m->reply_code, (int)m->reply_text.len, (char *)m->reply_text.bytes);
                        break;
                        }

                    default:
                        _snprintf_s(error_buffer,sizeof(error_buffer), _TRUNCATE, "AMPQ ERROR. %s: server error, method id 0x%08X\n", context, x.reply.id);
                        break;

                }
                break;
        }
        return 0;
    }

    bool set_ampq_error(int x, char const *context) {
        if (x < 0) 
            _snprintf_s(error_buffer, sizeof(error_buffer), _TRUNCATE, "AMPQ ERROR. %s: %s", context, amqp_error_string2(x));
        return (x==0);
    }

    bool check_socket() {
        if (!socket) 
            socket = amqp_tcp_socket_new(conn);
        return (socket!=NULL);
    }

    bool socket_open() {
        if (flag_init) 
            return 1;
        check_socket();
        int status = amqp_socket_open(socket, m_host.value.string, m_port.value.intval);
        if (status==AMQP_STATUS_OK){
            amqp_rpc_reply_t r=
            amqp_login(conn,                          // state the connection object
                       "/",                           // vhost the virtual host to connect to on the broker. The default on most brokers is "/"
                       0,                             // channel_max the limit for the number of channels for the connection 0 means no limit, and is a good default
                       131072,                        // frame_max the maximum size of an AMQP frame ont he wire to request of the broker for this connection. 4096 is the minimum size, 2^31-1 is the maximum, a good default is 131072 (128KB), or AMQP_DEFAULT_FRAME_SIZE
                       m_heartbeat.value.intval,      // heartbeat the number of seconds between heartbeat frame to request of the broker. A value of 0 disables heartbeats.
                       AMQP_SASL_METHOD_PLAIN,        // properties a table of properties to send the broker.
                       m_user.value.string, 
                       m_pass.value.string);
            if(!set_socket_error(r,"Logging in"))
                return 0;

            amqp_channel_open(conn, 1);

            if(!set_socket_error(amqp_get_rpc_reply(conn),"Opening channel"))
                return 0;

            flag_init=1;
            return 1;
            }
        else {
            return 0;
            }
    }

    void close_socket() {
        if (flag_init) {
            if(!set_socket_error(amqp_channel_close(conn, 1, AMQP_REPLY_SUCCESS),"Closing channel"   )) return;
            if(!set_socket_error(amqp_connection_close(conn, AMQP_REPLY_SUCCESS),"Closing connection")) return;
            set_ampq_error(amqp_destroy_connection(conn), "Ending connection");
        }
    }

    /*
    {
        try {
            map.at(13);
        }
    catch(const std::out_of_range& ex)
    {
        std::cout << "1) out_of_range::what(): " << ex.what() << '\n';
    }
    }
    */

    // сохраняет во внутреннем хранилище пользовательские заголовки но только с форматом строка
    void read_headers(amqp_table_t h) {
        str_headers.clear();

        for (int i = 0; i < h.num_entries; i++) {
            std::string header_key   = nupogodiAmpqPropToString(h.entries[i].key);
            if (h.entries[i].value.kind == AMQP_FIELD_KIND_BYTES || h.entries[i].value.kind == AMQP_FIELD_KIND_UTF8) {
                std::string header_value = nupogodiAmpqPropToString(h.entries[i].value.value.bytes);
                str_headers[header_key] = header_value;
            } else {
                // print("header %s has wrong type %c\n", header_key.c_str(), h.entries[i].value.kind);
            }
        }
    }
    



public:
    __int64 to_deftype(VALUE *v){
        switch (v->v_type) {
            case V_INTEGER:
                return 10000*(__int64)v->value.intval;
            case V_DOUBLE:
                return (__int64)(v->value.doubval*10000);
            case V_DOUBLEL:
                return (__int64)(v->value.doubvalL*10000);
#ifdef USE_FDECIMAL
            case V_MONEY:
                return v->value.i64val; //???
            case V_MONEYL:
                return v->value.i64val; //???
#else
            case V_MONEY:
                return v->value.monval;
            case V_MONEYL:
                return v->value.monval;
#endif
            case V_STRING:
                return (__int64)(atof(v->value.string)*10000);
            default: 
                return 0;
            }
        }

    /*
    std::string to_stdstr(VALUE *v){
        switch (v->v_type) {
            case V_INTEGER:
                return to_stdstr10000*(__int64)v->value.intval;
            case V_DOUBLE:
                return (__int64)(v->value.doubval*10000);
            case V_DOUBLEL:
                return (__int64)(v->value.doubvalL*10000);
            case V_MONEY:
                return v->value.i64val; //???
            case V_MONEYL:
                return v->value.i64val; //???
            case V_STRING:
                return (__int64)(atof(v->value.string)*10000);
            default: 
                return 0;
            }
        }
   */


   TNuPogodi (TGenObject *pThis = NULL) {

      ValueMake (&m_host);
      ValueSet (&m_host,V_STRING,"localhost");

      ValueMake (&m_port);
      m_port.v_type=V_INTEGER;
      m_port.value.intval=5672;
      
      ValueMake (&m_user);
      ValueSet (&m_user,V_STRING,"guest");

      ValueMake (&m_pass);
      ValueSet (&m_pass,V_STRING,"guest");

      ValueMake (&m_heartbeat);
      m_heartbeat.v_type=V_INTEGER;
      m_heartbeat.value.intval = 0;
      
      ValueMake (&m_exch);
      ValueSet (&m_exch,V_STRING,"");

      ValueMake (&m_auto_ack);
      m_auto_ack.v_type=V_INTEGER;
      m_auto_ack.value.intval=1;

      ValueMake (&m_no_ack);
      m_no_ack.v_type=V_BOOL;
      m_no_ack.value.boolval = 0;

      ValueMake (&m_last_result);
      m_last_result.v_type=V_INTEGER;
      m_last_result.value.intval = 0;

      ValueMake (&m_library_error);
      m_library_error.v_type=V_INTEGER;
      m_library_error.value.intval = 0;

      ValueMake (&m_queue_timeout);
      m_queue_timeout.v_type=V_INTEGER;
      m_queue_timeout.value.intval = 60;

      ValueMake(&m_content_type);
      m_content_type.v_type = V_UNDEF;

      ValueMake(&m_type);
      m_type.v_type = V_UNDEF;

      ValueMake(&m_reply_to);
      m_reply_to.v_type = V_UNDEF;
      
      //ValueMake (&m_error);
      //m_error.v_type=V_UNDEF;

      m_error.v_type=V_STRING;
      m_error.value.string=error_buffer;
      *error_buffer='\0';

      sprintf(consumer_tag, "rsl%08i", UserNumber());      

      }

   ~TNuPogodi () {
      ValueClear (&m_host);
      ValueClear (&m_port);
      ValueClear (&m_user);
      ValueClear (&m_pass);
      ValueClear (&m_heartbeat);
      ValueClear (&m_exch);
      ValueClear (&m_error);
      ValueClear (&m_auto_ack);
      ValueClear (&m_no_ack);
      ValueClear (&m_last_result);
      ValueClear (&m_library_error);

      close_socket();
      }


    RSL_CLASS(TNuPogodi)


    RSL_INIT_DECL() {         // void TNuPogodi::Init (int *firstParmOffs)
        VALUE *v;
        GetParm (*firstParmOffs,&v);
      
        conn=amqp_new_connection();
        //amqp_socket_t *socket = amqp_tcp_socket_new(conn);
        check_socket();
    }
                  

    RSL_METHOD_DECL(SendFile) {
        ValueClear (retVal);
        retVal->v_type = V_BOOL;
        retVal->value.boolval=0;

        if (socket_open()) {
            char * messagefilename = rsGetFilePathParam(1);
            char * messagebody;
            char * routing_key = rsGetStringParam(2,"");
            if(messagebody =nupogodiGetFileContent(messagefilename)) {

                amqp_basic_properties_t props;
                props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
                if (m_content_type.v_type == V_STRING && m_content_type.value.string) {
                    props.content_type = amqp_cstring_bytes(m_content_type.value.string);
                    // props.content_type = amqp_bytes_malloc_dup(m_content_type.value.string);
                } else {
                    props.content_type = amqp_cstring_bytes("text/plain");
                }

                if (m_type.v_type == V_STRING && m_type.value.string) {
                    props.type = amqp_cstring_bytes(m_type.value.string);
                    props._flags |= AMQP_BASIC_TYPE_FLAG;
                    // props.content_type = amqp_bytes_malloc_dup(m_type.value.string);
                } else {
                    props.type.bytes = NULL;
                    props.type.len = 0;
                }

                props.delivery_mode = 2; /* persistent delivery mode */
                if(set_ampq_error(amqp_basic_publish(conn, 1, amqp_cstring_bytes(m_exch.value.string),
                                                    amqp_cstring_bytes(routing_key), 0, 0,
                                                    &props, amqp_cstring_bytes(messagebody)),
                                 "Send")) {
                    retVal->value.boolval=1;
                }

                free(messagebody);

            }
        }
        return 0;
    }


    RSL_METHOD_DECL(SendText) {
        ValueClear (retVal);
        retVal->v_type = V_BOOL;
        retVal->value.boolval=0;

        if (socket_open()) {
            char * message_in = rsGetStringParam(1,"");                     // текст из RS в кодировке 866

            int message_len=strlen(message_in);                            // кол-во символов
            wchar_t * message_buff= (wchar_t *)malloc(2*(message_len+1));  // промежуточный буфер в UTF-16
            MultiByteToWideChar( CP_OEMCP, 0, message_in, -1, message_buff, message_len+1);        // 866 -> UTF-16
            size_t messagebody_len=WideCharToMultiByte( CP_UTF8, 0, message_buff, -1, NULL, 0, 0, 0); // определяем, сколько байт будет занимать сообщение в UTF-8
            char * messagebody = (char *)malloc(messagebody_len);        // сообщения для передачи в кролика в UTF-8
            WideCharToMultiByte( CP_UTF8, 0, message_buff, -1, messagebody, messagebody_len, 0, 0);  // UTF-16 -> UTF-8
            free(message_buff);

            char * routing_key = rsGetStringParam(2,"");
            { // 

                amqp_basic_properties_t props;
                props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
                if (m_content_type.v_type == V_STRING && m_content_type.value.string) {
                    props.content_type = amqp_cstring_bytes(m_content_type.value.string);
                    // props.content_type = amqp_bytes_malloc_dup(m_content_type.value.string);
                } else {
                    props.content_type = amqp_cstring_bytes("text/plain");
                }

                if (m_type.v_type == V_STRING && m_type.value.string) {
                    props.type = amqp_cstring_bytes(m_type.value.string);
                    props._flags |= AMQP_BASIC_TYPE_FLAG;
                    // props.content_type = amqp_bytes_malloc_dup(m_type.value.string);
                } else {
                    props.type.bytes = NULL;
                    props.type.len = 0;
                }

                // отправка пользовательких заголовков
                if (str_headers.size() > 0) {
                    props._flags |= AMQP_BASIC_HEADERS_FLAG;
                    props.headers.num_entries = str_headers.size();
                    props.headers.entries     = (amqp_table_entry_t_ *)calloc(props.headers.num_entries, sizeof(amqp_table_entry_t));
                    int i = 0;  
                    for (auto it = str_headers.cbegin(); it != str_headers.cend(); ++it) {
                        // (*it).first << ':' << (*it).second << ']';
                        props.headers.entries[i].key = amqp_cstring_bytes((*it).first.c_str());
                        props.headers.entries[i].value.kind  = AMQP_FIELD_KIND_UTF8;
                        props.headers.entries[i].value.value.bytes = amqp_cstring_bytes((*it).second.c_str());

                        i++;
                    }
                }

                props.delivery_mode = 2; /* persistent delivery mode */
                if(set_ampq_error(amqp_basic_publish(conn, 1, amqp_cstring_bytes(m_exch.value.string),
                                                    amqp_cstring_bytes(routing_key), 0, 0,
                                                    &props, amqp_cstring_bytes(messagebody)),
                                 "Send")) {
                    retVal->value.boolval=1;
                }

                free(messagebody);

            }
        }
        return 0;
    }

    RSL_METHOD_DECL(OpenQueue) {
        ValueClear (retVal);
        char * queue_name = rsGetStringParam(1, NULL); // нет значения по умолчанию

        flag_queue_opened = 0;
        if (socket_open()) {

            amqp_basic_consume(conn,                            // state connection state
                               1,                               // channel the channel to do the RPC on
                               amqp_cstring_bytes(queue_name),  // queue
                               amqp_cstring_bytes(consumer_tag), // consumer_tag (was amqp_empty_bytes)
                               0,                               // no_local
                               m_no_ack.value.boolval,          // no_ack
                               0,                               // exclusive
                               amqp_empty_table);               // arguments

            if (set_socket_error(amqp_get_rpc_reply(conn), "Consuming"))
                flag_queue_opened = 1;

        }

        ReturnVal (V_BOOL, &flag_queue_opened);

        return 0;
    }

    RSL_METHOD_DECL(ReadQueue) {
        ValueClear (retVal);

        if (!flag_queue_opened) {
            _snprintf_s(error_buffer,sizeof(error_buffer), _TRUNCATE, "NUPOGODI ERROR. use OpenQueue before ReadQueue");
            return 0;
        }

        last_delivery_tag = 0;
        m_last_result.value.intval = 0;
        m_library_error.value.intval = 0;

        struct timeval timeout;
        //memset(&timeout,0,sizeof(timeout));
        timeout.tv_sec = m_queue_timeout.value.intval;
        timeout.tv_usec = 0;

        amqp_rpc_reply_t res;
        amqp_envelope_t envelope;

        amqp_maybe_release_buffers(conn);

        res = amqp_consume_message(conn, &envelope, &timeout, 0);
           
        m_last_result.value.intval = res.reply_type; // сохраняем полученное значение кода возврата в свойство
        m_library_error.value.intval = res.library_error;

        set_socket_error(res, "Getting envelop from queue");

        if (AMQP_RESPONSE_NONE == res.reply_type) {
            return 0;
        }

        if (AMQP_RESPONSE_LIBRARY_EXCEPTION == res.reply_type &&
            AMQP_STATUS_UNEXPECTED_STATE    == res.library_error) {
            amqp_frame_t frame;
            amqp_simple_wait_frame(conn, &frame);
            return 0;
        }

        if (AMQP_RESPONSE_NORMAL != res.reply_type) {
            return 0;
        }

        last_delivery_tag = envelope.delivery_tag;
        strncpy(last_routing_key, (char *)envelope.routing_key.bytes, min(envelope.routing_key.len, sizeof(last_routing_key)-1));
        last_routing_key[min(envelope.routing_key.len, sizeof(last_routing_key)-1)] = '\0';
        strncpy(last_exchange,    (char *)envelope.exchange.bytes,    min(envelope.exchange.len,    sizeof(last_exchange)-1));
        last_exchange[min(envelope.exchange.len, sizeof(last_exchange)-1)] = '\0';

        //print("Delivery %u, exchange %.*s routingkey %.*s\n",
        //       (unsigned)envelope.delivery_tag, (int)envelope.exchange.len,
        //       (char *)envelope.exchange.bytes, (int)envelope.routing_key.len,
        //       (char *)envelope.routing_key.bytes);

        if (envelope.message.properties._flags & AMQP_BASIC_CONTENT_TYPE_FLAG) {
            nupogodiAmpqPropToRsVal(envelope.message.properties.content_type, &m_content_type);
            //ValueClear(&m_content_type);
            //ValueSet(&m_content_type, V_STRING, envelope.message.properties.content_type.bytes);
        } else {
            int v = 0;
            ValueSet(&m_content_type, V_UNDEF, &v);
        }

        if (envelope.message.properties._flags & AMQP_BASIC_REPLY_TO_FLAG) {
            nupogodiAmpqPropToRsVal(envelope.message.properties.reply_to, &m_reply_to);
            // ValueClear(&m_reply_to);
            // ValueSet(&m_reply_to, V_STRING, envelope.message.properties.reply_to.bytes);
        } else {
            int v = 0;
            ValueSet(&m_reply_to, V_UNDEF, &v);
        }

        read_headers(envelope.message.properties.headers);

        //if (envelope.message.properties._flags & AMQP_BASIC_CONTENT_TYPE_FLAG) {
        //  print("Content-type: %.*s\n",
        //         (int)envelope.message.properties.content_type.len,
        //         (char *)envelope.message.properties.content_type.bytes);
        //}

        if (AMQP_RESPONSE_NORMAL == res.reply_type) {

            int char_count = MultiByteToWideChar(CP_UTF8, 0, (char *)envelope.message.body.bytes, envelope.message.body.len, NULL, 0);
            if(char_count > 0 && char_count != 0xFFFD) {
                wchar_t * wbuff= (wchar_t *)malloc(char_count*sizeof(wchar_t));
                MultiByteToWideChar(CP_UTF8, 0, (char *)envelope.message.body.bytes, envelope.message.body.len, wbuff, char_count);
                char * message_buff=(char *)malloc((char_count+1)*sizeof(char));
                WideCharToMultiByte(866, 0, wbuff, char_count, message_buff, char_count, 0, 0);
                message_buff[char_count]='\0';
                free(wbuff);
                ValueSet (retVal,V_STRING,(void *)message_buff);
                free(message_buff);
            } else {
                ValueSet (retVal,V_STRING,"");
            }

            if (m_auto_ack.value.intval) {
                if (0 == amqp_basic_ack(conn, 1, last_delivery_tag, false)) {
                    last_delivery_tag = 0;
                }
            }

        }

        amqp_destroy_envelope(&envelope);

        return 0;
    }

    RSL_METHOD_DECL(CancelQueue) {
        ValueClear (retVal);
        int rsl_result = 0;

        if (!flag_queue_opened) {
            _snprintf_s(error_buffer,sizeof(error_buffer), _TRUNCATE, "NUPOGODI ERROR. use OpenQueue before CancelQueue");
            return 0;
        }

        amqp_basic_cancel_ok_t * result;
        result = amqp_basic_cancel(conn, 1, amqp_cstring_bytes(consumer_tag));
        if (result) {
            flag_queue_opened = 0;
            rsl_result = 1;
        }

        ReturnVal (V_BOOL, &rsl_result);

        return 0;
    }



    RSL_METHOD_DECL(Ack) {
        ValueClear (retVal);
        int ret = 0;
        if (last_delivery_tag) {
            if (0 == amqp_basic_ack(conn, 1, last_delivery_tag, false)) {
                last_delivery_tag = 0;
                ret = 1;
            }
        }
        ReturnVal (V_BOOL, &ret);
        return 0;
    }

    RSL_METHOD_DECL(ReadMessage) {
        ValueClear (retVal);
        last_delivery_tag = 0;
        char * queue_name = rsGetStringParam(1,NULL); // нет значения по умолчанию
        if (socket_open()) {

            amqp_rpc_reply_t res = amqp_basic_get(conn, 1, amqp_cstring_bytes(queue_name), m_auto_ack.value.intval);

            if (res.reply.id == AMQP_BASIC_GET_EMPTY_METHOD) {
                return 0;
            }

            amqp_message_t message;
            amqp_rpc_reply_t reply = amqp_read_message(conn, 1, &message, 0);
            if (AMQP_RESPONSE_NORMAL != reply.reply_type) {
                return 0;
            }

            int char_count = MultiByteToWideChar(CP_UTF8, 0, (char *)message.body.bytes, message.body.len, NULL, 0);
            if (char_count > 0 && char_count != 0xFFFD) {
                wchar_t * wbuff= (wchar_t *)malloc(char_count*sizeof(wchar_t));
                MultiByteToWideChar(CP_UTF8, 0, (char *)message.body.bytes, message.body.len, wbuff, char_count);
                char * message_buff=(char *)malloc((char_count+1)*sizeof(char));
                WideCharToMultiByte(866, 0, wbuff, char_count, message_buff, char_count, 0, 0);
                message_buff[char_count]='\0';
                free(wbuff);
                ValueSet(retVal, V_STRING, (void *)message_buff);
            } else {
                ValueSet(retVal, V_STRING, "");
            }

            if (message.properties._flags & AMQP_BASIC_CONTENT_TYPE_FLAG) {
                //  print("Content-type: %.*s\n", (int)message.properties.content_type.len, (char *)message.properties.content_type.bytes);
                nupogodiAmpqPropToRsVal(message.properties.content_type, &m_content_type);

            } else {
                int v = 0;
                ValueSet(&m_content_type, V_UNDEF, &v);
            }

            if (message.properties._flags & AMQP_BASIC_REPLY_TO_FLAG) {
                nupogodiAmpqPropToRsVal(message.properties.reply_to, &m_reply_to);
            } else {
                int v = 0;
                ValueSet(&m_reply_to, V_UNDEF, &v);
            }


            if (message.properties._flags & AMQP_BASIC_TYPE_FLAG) {
                nupogodiAmpqPropToRsVal(message.properties.type, &m_type);
            } else {
                int v = 0;
                ValueSet(&m_type, V_UNDEF, &v);
            }

            read_headers(message.properties.headers);

            amqp_destroy_message(&message);
            return 0;
        }
        int ret=0;
        ValueSet (retVal,V_BOOL,(void *)&ret);
        return 0;
    }

    RSL_METHOD_DECL(GetHeader) {
        ValueClear (retVal);

        char * header_name = rsGetFilePathParam(1);

        try {
            std::string ret_string = str_headers.at(std::string(header_name));
            ValueSet (retVal, V_STRING, (void *)ret_string.c_str());
        }
        catch(const std::out_of_range& ex) {
            // ничего не делаем, возвращаем null
        }

        return 0;
    }

    RSL_METHOD_DECL(ClearHeaders) {
        ValueClear (retVal);
        str_headers.clear();
        return 0;
    }

    RSL_METHOD_DECL(AddHeader) {
        ValueClear (retVal);
        char * szKey = rsGetStringParam(1, NULL);
        char * szVal = rsGetStringParam(2, NULL);
        str_headers[std::string(szKey)] = std::string(szVal);
        return 0;
    }

    RSL_METHOD_DECL(GetHeadersCount) {
        ValueClear (retVal);
        retVal->v_type = V_INTEGER;
        retVal->value.intval = str_headers.size();

        return 0;
    }



    RSL_GETPROP_DECL(RouteKey)  { 
        VALUE * pv = PushValue(NULL);
        ValueSet (pv, V_STRING, (void *)last_routing_key);
        ReturnVal2 (pv);
        PopValue();
        return 0;
    }

    
    RSL_METHOD_DECL(TestParam) {
        VALUE *vParm;
       
        if (GetParm (1,&vParm)     && vParm->v_type     == V_STRING ){
            print (vParm->value.string);
            print ("\n");
            }
        else {
            print ("no param\n");
            }
        return 0;
    }


private:
    VALUE m_host;
    VALUE m_port;
    VALUE m_user;
    VALUE m_pass;
    VALUE m_heartbeat;
    VALUE m_error;
    VALUE m_exch;
    VALUE m_auto_ack; // для чтения очереди 1 - сообщения подтверждаются при получении, 0 - после обработки надо вызывать Ack
    VALUE m_no_ack;   // для открытия очереди 0 - def
    VALUE m_last_result;
    VALUE m_queue_timeout;  // после этого ожидания управление возвращается в RSL, даже если сообщение не получено
    VALUE m_library_error;
    VALUE m_content_type;
    VALUE m_type;
    VALUE m_reply_to;
    char error_buffer[256];
    amqp_connection_state_t conn;
    amqp_socket_t * socket = NULL;
    bool flag_init=0;
    uint64_t last_delivery_tag = 0;  // delivery_tag последнего полученного сообщения
    char last_routing_key[256];  // 
    char last_exchange[256];  // 
    char consumer_tag[32];
    int flag_queue_opened = 0;
    std::map<std::string, std::string> str_headers; 

};

TRslParmsInfo prmOneStr[] = { {V_STRING,0} };
TRslParmsInfo prmTwoStr[] = { {V_STRING,0},{V_STRING,0} };
TRslParmsInfo prmNo[] = {{}};

RSL_CLASS_BEGIN(TNuPogodi)
    RSL_PROP_EX    (host,           m_host,          -1, V_STRING,  0)
    RSL_PROP_EX    (port,           m_port,          -1, V_INTEGER, 0)
    RSL_PROP_EX    (user,           m_user,          -1, V_STRING,  0)
    RSL_PROP_EX    (pass,           m_pass,          -1, V_STRING,  0)
    RSL_PROP_EX    (heartbeat,      m_heartbeat,     -1, V_INTEGER, 0)
    RSL_PROP_EX    (exch,           m_exch,          -1, V_STRING,  0)
    RSL_PROP_EX    (AutoAck,        m_auto_ack,      -1, V_INTEGER, 0)
    RSL_PROP_EX    (NoAck,          m_no_ack,        -1, V_BOOL,    0)
    RSL_PROP_EX    (QueueTimeout,   m_queue_timeout, -1, V_INTEGER, 0)
    RSL_PROP_EX    (ContentType,    m_content_type,  -1, V_STRING,  0)
    RSL_PROP_EX    (Type,           m_type,          -1, V_STRING,  0)
    RSL_PROP_EX    (ReplyTo,        m_reply_to,      -1, V_STRING,  0)

    RSL_PROP_EX    (error,          m_error,         -1, V_STRING,  VAL_FLAG_RDONLY)
    RSL_PROP_EX    (LastResultCode, m_last_result,   -1, V_INTEGER, VAL_FLAG_RDONLY)
    RSL_PROP_EX    (LibraryError,   m_library_error, -1, V_INTEGER, VAL_FLAG_RDONLY)

    RSL_METH_EX    (SendFile,    -1, V_BOOL,  0, RSLNP(prmOneStr), prmOneStr)
    RSL_METH_EX    (SendText,    -1, V_BOOL,  0, RSLNP(prmOneStr), prmOneStr)
    RSL_METH_EX    (Ack,         -1, V_BOOL,  0, RSLNP(prmOneStr), prmOneStr)
    RSL_METH_EX    (OpenQueue,   -1, V_BOOL,  0, RSLNP(prmOneStr), prmOneStr)
    RSL_METH_EX    (ReadQueue,   -1, V_UNDEF, 0, RSLNP(prmOneStr), prmOneStr)
    RSL_METH_EX    (ReadMessage, -1, V_UNDEF, 0, RSLNP(prmOneStr), prmOneStr)
    RSL_METH_EX    (CancelQueue, -1, V_BOOL,  0, RSLNP(prmNo),     prmNo)
    RSL_METH_EX    (GetHeader,   -1, V_UNDEF, 0, RSLNP(prmOneStr), prmOneStr)
    RSL_METH_EX    (ClearHeaders,-1, V_UNDEF, 0, RSLNP(prmNo),     prmNo)
    RSL_METH_EX    (AddHeader,   -1, V_UNDEF, 0, RSLNP(prmTwoStr), prmTwoStr)
    RSL_METH_EX    (GetHeadersCount, -1, V_INTEGER, 0, RSLNP(prmNo), prmNo)

    RSL_PROP_METH  (RouteKey)

    RSL_INIT
RSL_CLASS_END  



EXP32 void DLMAPI EXP AddModuleObjects (void) {
    RslAddUniClass (TNuPogodi::TablePtr,true);
    }





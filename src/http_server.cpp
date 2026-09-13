#include <iostream>
#include <string>
#include "http_server.hpp"
#include "json.hpp"
#include "song.hpp"
//#include "queue.hpp"
#include "mpdqueue.hpp"
#include "library.hpp"


using json = nlohmann::json;

Mpdqueue SQ;
Library LQ;

/*
 * 队列/播放状态的唯一真相是 mpd，内部队列 SQ 只是 mpd 队列的镜像。
 * 轮询线程 (mpd_poll) 在连接成功、以及 mpd queue_version 变化（外部 mpc /
 * 其它客户端改动）时会调用 queue_sync_from_mpd() 重建 SQ；所以 HTTP 侧对
 * SQ 的读写都用 mpd_lock 串行化，避免重建过程中读到半成品 vector。
 * mpd_lock 是递归锁，与 mpd_poll 持锁调用兼容。
 */
extern "C" void queue_sync_from_mpd(void) {
    SQ.syncFromMpd();
}

// SQ 的 JSON 快照，读操作与同步线程串行化
static std::string queueJson() {
    mpd_lock();
    std::string j = SQ.toJson();
    mpd_unlock();
    return j;
}

/*
 * JSON bodies come straight from the network (unauthenticated LAN clients), so
 * every field is checked for presence and type before use. A malformed body is
 * a 400, never an escaped json::exception (which terminated the process, i.e. a
 * trivial remote DoS).
 */

//解析请求 body 为 JSON；失败返回 false（调用方回 400）
static bool parseBody(struct mg_http_message *hm, json &out){
    try {
        std::string json_str = std::string(hm->body.buf,(int) hm->body.len);
        out = json::parse(json_str);
        return true;
    } catch (const json::exception& e) {
        std::cerr << "JSON parse error: " << e.what() << std::endl;
        return false;
    }
}

//查询某专辑的数据，id在post数据中；body 不合法返回 false
bool queryAlbum(struct mg_http_message *hm, std::string &out){
    json j;
    if(!parseBody(hm, j))
        return false;
    if(!j.is_object() || !j.contains("album") || !j["album"].is_string())
        return false;

    out = LQ.AlbumToJson(j["album"].get<std::string>());
    return true;
}
//根据post内容加入track到mpd的queue，并且返回第一个加入的queue_sid；body 不合法时 ok=false
int addTrackTompd(struct mg_http_message *hm, bool &ok){
    json jsonArray;
    int queue_sid = -1;
    int queue_sid_t = -1;

    ok = false;
    if(!parseBody(hm, jsonArray))
        return -1;
    // 检查解析结果是否为数组
    if (!jsonArray.is_array()) {
        std::cerr << "jsonArray not is_array" <<std::endl;
        return -1;
    }
    ok = true;

    if(SQ.length() == 0) //如果SQ中没有歌曲，清空mpd的queue，以保持mpd和SQ的同步。
    {
        mpd_lock();
        mpd_run_clear(mpd.conn);
        mpd_unlock();
    }

    // 遍历 JSON 数组
    for (const auto& item : jsonArray) {
        // 检查每个元素是否为对象
        if (item.is_object()) {
            std::string id = "";
            std::string title = "";
            std::string album = "";
            std::string artist = "";
            std::string url = "";
            std::string coverArt = "";
            int duration = 0;
            int track = 0;
            int year = 0;
            // 检查键是否存在并提取值
            if (item.contains("id") && item["id"].is_string())
                id = item["id"].get<std::string>();
            if (item.contains("title") && item["title"].is_string())
                title = item["title"].get<std::string>();
            if (item.contains("album") && item["album"].is_string())
                album = item["album"].get<std::string>();
            if (item.contains("artist") && item["artist"].is_string())
                artist = item["artist"].get<std::string>();
            if (item.contains("url") && item["url"].is_string())
                url = item["url"].get<std::string>();
            if (item.contains("duration") && item["duration"].is_number_integer())
                duration = item["duration"].get<int>();
            if (item.contains("track") && item["track"].is_number_integer())
                track = item["track"].get<int>();
            if (item.contains("year") && item["year"].is_number_integer())
                year = item["year"].get<int>();
            if (item.contains("coverArt") && item["coverArt"].is_string())
                coverArt = item["coverArt"].get<std::string>();                
            Song asong = Song(id, artist, title, album, url);
            asong.setDuration(duration);
            asong.setTrack(track);
            asong.setYear(year);
            asong.setCoverart(coverArt);
            queue_sid = SQ.addSong(asong);
            if((queue_sid != -1) && (queue_sid_t == -1))//记住第一个加入queue的id，作为返回值
                queue_sid_t = queue_sid;
        }
    }
    //根据当前queue补全song position信息
    SQ.makeupPos();
    return  queue_sid_t;
}
//根据post的track，删除该track在queue中；body 不合法返回 false
bool rmTrackBySid(struct mg_http_message *hm){
    json j;
    if(!parseBody(hm, j))
        return false;
    if(!j.is_object() || !j.contains("queue_sid") || !j["queue_sid"].is_number_integer())
        return false;

    SQ.removeSongBySid(j["queue_sid"].get<int>());
    return true;
}

// 400 响应：body 缺失/字段缺失/类型不符
static void replyBadRequest(struct mg_connection *c){
    mg_http_reply(c, 400, "Content-Type: application/json\r\n", "%s",
        "{\"error\":\"invalid request body\"}");
}

void callback_http(struct mg_connection *c,struct mg_http_message *hm,const char *s_root_dir)
{
    int queue_sid;

    /* 已知 /api/* 路由的方法校验：方法不匹配时必须**明确回包**。
       原先这些分支直接 return 不回包，调用方随后会在日志里读空的响应缓冲
       （main.cpp 的 c->send.buf + 9）→ 越界读 → SIGSEGV。
       局域网内任何一次错误方法的请求（甚至浏览器预取）都能把服务打崩。 */
    {
        bool is_post_route = mg_match(hm->uri, mg_str("/api/queue/add/*"), NULL) ||
                             mg_match(hm->uri, mg_str("/api/queue/replace/*"), NULL) ||
                             mg_match(hm->uri, mg_str("/api/queue/del"), NULL) ||
                             mg_match(hm->uri, mg_str("/api/library"), NULL);
        bool is_get_route  = mg_match(hm->uri, mg_str("/api/hello"), NULL) ||
                             mg_match(hm->uri, mg_str("/api/queue"), NULL) ||
                             mg_match(hm->uri, mg_str("/api/library"), NULL);
        bool method_ok = (is_post_route && mg_match(hm->method, mg_str("POST"), NULL)) ||
                         (is_get_route  && mg_match(hm->method, mg_str("GET"), NULL));
        if ((is_post_route || is_get_route) && !method_ok) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n",
                          "{\"error\":\"method not allowed\"}\n");
            return;
        }
    }

    if (mg_match(hm->uri, mg_str("/api/hello"), NULL)) {              // REST API call?
        mg_http_reply(c, 200, "", "{%m:%d}\n", MG_ESC("status"), 1);    // Yes. Respond JSON
    }else if(mg_match(hm->uri, mg_str("/api/queue/add/*"), NULL)) {//将浏览器发送的json转成song并加入mpd queue
        if(mg_match(hm->method, mg_str("POST"),NULL)){
            //printf("POST request body: %.*s\n", (int) hm->body.len, hm->body.buf);
             bool ok = false;
             mpd_lock();//与轮询线程的队列同步串行化
             queue_sid = addTrackTompd(hm, ok);//添加track到queue
            if(!ok){
                mpd_unlock();
                replyBadRequest(c);
                return;
            }
            if(mg_match(hm->uri, mg_str("/api/queue/add/play"), NULL))
               SQ.playSid(queue_sid);//播放
            std::string qj = SQ.toJson();
            mpd_unlock();
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", qj.c_str());
        }
    }else if(mg_match(hm->uri, mg_str("/api/queue/replace/*"), NULL)) {//将浏览器发送的json转成song并替换mpd queue
        if(mg_match(hm->method, mg_str("POST"),NULL)){
            //printf("POST request body: %.*s\n", (int) hm->body.len, hm->body.buf);
            bool ok = false;
            mpd_lock();//与轮询线程的队列同步串行化
            SQ.clear();
            addTrackTompd(hm, ok);
            if(!ok){
                mpd_unlock();
                replyBadRequest(c);
                return;
            }
            if(mg_match(hm->uri, mg_str("/api/queue/replace/play"), NULL))
                SQ.playPos(0);
            std::string qj = SQ.toJson();
            mpd_unlock();
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", qj.c_str());
        }
    }else if(mg_match(hm->uri, mg_str("/api/queue/del"), NULL)) { //删除某歌曲
        if(mg_match(hm->method, mg_str("POST"),NULL)){
            //printf("POST request body: %.*s\n", (int) hm->body.len, hm->body.buf);
            mpd_lock();//与轮询线程的队列同步串行化
            bool ok = rmTrackBySid(hm);
            std::string qj = SQ.toJson();
            mpd_unlock();
            if(!ok){
                replyBadRequest(c);
                return;
            }
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", qj.c_str());
        }                      
    }else if(mg_match(hm->uri, mg_str("/api/queue"), NULL)) { //查询队列
        if(mg_match(hm->method, mg_str("GET"),NULL)){
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", queueJson().c_str());
        }
    }else if(mg_match(hm->uri, mg_str("/api/library"), NULL)) { //查询专辑列表
        if(mg_match(hm->method, mg_str("GET"),NULL)){
            LQ.clear();// clear first
            LQ.getMpdDB();
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", LQ.allAlbumToJson().c_str());
        }
        if(mg_match(hm->method, mg_str("POST"),NULL)){//查询某专辑，查询id在post中
            std::string albumJson;
            if(!queryAlbum(hm, albumJson)){
                replyBadRequest(c);
                return;
            }
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", albumJson.c_str());
        }
    }else{
        struct mg_http_serve_opts opts = {.root_dir = s_root_dir};  // For all other URLs,
        mg_http_serve_dir(c, hm, &opts);                     // Serve static files
    }
    //mg_send_status(c, 404);
    //mg_printf_data(c, "Not Found");
}
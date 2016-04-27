/***************************************************************************
 *
 * Copyright (c) 2016 aishuyu, Inc. All Rights Reserved
 *
 **************************************************************************/



/**
 * @file connection_timer_manager.cpp
 * @author aishuyu(asy5178@163.com)
 * @date 2016/03/08 23:46:33
 * @brief
 *
 **/


#include "connection_timer_manager.h"

#include <time.h>
#include <algorithm>

#include "util/rpc_util.h"


namespace libevrpc {

using std::string;

ConnectionTimerManager::ConnectionTimerManager() :
    connection_buf_ptr_(new CTHM_VEC()),
    connection_del_list_ptr_(new INT_LIST_PTR_LIST()),
    connection_buf_mutex_ptr_(new MUTEX_VEC()),
    connection_bucket_mutex_ptr_(new MUTEX_VEC()),
    connection_dellist_mutex_ptr_(new MUTEX_VEC()),
    refresh_client_list_ptr_(new INT_LIST()),
    buf_index_(0),
    bucket_index_(0),
    refresh_interval_(30),
    running_(false) {
    for (int32_t i = 0; i < 60; ++i) {
        connection_pool_buckets_[i] = NULL;
    }
}

ConnectionTimerManager::~ConnectionTimerManager() {
}

ConnectionTimerManager& ConnectionTimerManager::GetInstance() {
    static ConnectionTimerManager ctm_instance;
    return ctm_instance;
}

int32_t ConnectionTimerManager::InitTimerBuf() {
     CT_HASH_MAP_PTR ctm_ptr(new CT_HASH_MAP());
     int32_t cur_buf_index = 0;
     {
         MutexLockGuard lock(connection_pool_mutex_);
         connection_buf_ptr_->push_back(ctm_ptr);
         INT_LIST_PTR ilp(new INT_LIST());
         connection_del_list_ptr_->push_back(ilp);

         Mutex buf_mutex;
         connection_buf_mutex_ptr_->push_back(std::move(buf_mutex));
         Mutex bucket_mutex;
         connection_bucket_mutex_ptr_->push_back(std::move(bucket_mutex));
         Mutex dellist_mutex;
         connection_dellist_mutex_ptr_->push_back(std::move(dellist_mutex));
         cur_buf_index = buf_index_;
         ++buf_index_;
     }
     running_ = true;
     return cur_buf_index;
}

int32_t ConnectionTimerManager::InsertConnectionTimer(
        const string& ip_addr,
        int32_t fd,
        int32_t buf_index) {
    CT_PTR ct_ptr(new ConnectionTimer());
    ct_ptr->fd = fd;
    ct_ptr->client_addr = std::move(ip_addr);
    ct_ptr->expire_time = time(NULL) + refresh_interval_;

    /*
     * every thread has its own buf_index
     * mutex is shared with background thread
     */
    Mutex& mutex = connection_buf_mutex_ptr_->at(buf_index);
    {
        MutexLockGuard guard(mutex);
        CT_HASH_MAP_PTR& ctm_ptr = connection_buf_ptr_->at(buf_index);
        if (NULL == ctm_ptr) {
            /*
             * means the connection map is taken to in the connection
             * time wheel bucket
             */
            ctm_ptr.reset(new CT_HASH_MAP());
        }
        string ct_key = GenerateTimerKey(ip_addr, fd);
        ctm_ptr->insert(std::make_pair(std::move(ct_key), ct_ptr));
    }
    return 0;
}

void ConnectionTimerManager::DeleteConnectionTimer(
        const std::string& ip_addr,
        int32_t fd,
        int32_t buf_index) {
    string ct_key = GenerateTimerKey(ip_addr, fd);
    Mutex& mutex = connection_dellist_mutex_ptr_->at(buf_index);
    {
        MutexLockGuard guard(mutex);
        INT_LIST_PTR& ilp = connection_del_list_ptr_->at(buf_index);
        if (NULL == ilp) {
            ilp.reset(new INT_LIST());
        }
        ilp->push_back(std::move(ct_key));
    }
}

bool ConnectionTimerManager::InsertRefreshConnectionInfo(string& ip_addr) {
    MutexLockGuard guard(refresh_client_list_mutex_);
    if (NULL == refresh_client_list_ptr_) {
        refresh_client_list_ptr_.reset(new INT_LIST());
    }
    refresh_client_list_ptr_->push_back(std::move(ip_addr));
    return true;
}

void ConnectionTimerManager::Run() {
    bucket_index_ = 0;
    while (running_) {
        ConnectionBufCrawler();
        CT_MAP_PTR& ctm_ptr = connection_pool_buckets_[bucket_index_];
        if (NULL == ctm_ptr || ctm_ptr->empty()) {
            sleep(3);
            continue;
        }
        for (CT_MAP::iterator iter = ctm_ptr->begin(); iter != ctm_ptr->end(); ++iter) {
            CT_PTR& ct_ptr = iter->second;
            if (NULL == ct_ptr) {
                continue;
            }
            if (ct_ptr->expire_time > time(NULL)) {
                // TODO  if client is gone, disconnect it! and remove it
                //       if client is fine, expire_time += refresh_interval_
            } else {
                break;
            }
        }
        bucket_index_ = (bucket_index_ + 1) % buckets_size;
        sleep(1);
    }
}

/*
 * FIX ME
 */
string ConnectionTimerManager::GenerateTimerKey(const std::string& ip_addr, int32_t fd) {
    // uint32_t hash_code = BKDRHash(ip_addr.c_str());
    return ip_addr + "_" + std::to_string(fd);
}

int32_t ConnectionTimerManager::GenerateBucketNum(const string& ori_key) {
    return BKDRHash(ori_key.c_str()) % buckets_size;
}

bool ConnectionTimerManager::ConnectionBufCrawler() {
    if (NULL == connection_buf_ptr_) {
        return false;
    }

    /*
     * crawl all the connection timer from buffer into connection pool
     */
    for (int32_t buf_index = 0; buf_index < connection_buf_ptr_->size(); ++buf_index) {
        /*
         *  get the all connection been deleted
         */
        INT_LIST_PTR ilp = NULL;
        Mutex& del_mutex = connection_dellist_mutex_ptr_->at(buf_index);
        {
            MutexLockGuard guard(del_mutex);
            INT_LIST_PTR& tmp_ilp = connection_del_list_ptr_->at(buf_index);
            /*
             * take the del connection list
             */
            ilp = tmp_ilp;
            tmp_ilp = NULL;
        }

        /*
         * get connection from buf
         */
        CT_HASH_MAP_PTR local_ctm_ptr = NULL;
        Mutex& buf_mutex = connection_buf_mutex_ptr_->at(buf_index);
        {
            MutexLockGuard guard(buf_mutex);
            CT_HASH_MAP_PTR& ctm_ptr =  connection_buf_ptr_->at(buf_index);
            local_ctm_ptr = ctm_ptr;
            ctm_ptr = NULL;
        }
        /*
         * start to del the connection
         */
        if (NULL != ilp) {
            for (int32_t del_index = 0; del_index < ilp->size(); ++del_index) {
                string& del_key = ilp->at(del_index);
                /*
                 * delete the connction from buffer first
                 */
                if (NULL != local_ctm_ptr) {
                    CT_HASH_MAP::iterator ct_iter = local_ctm_ptr->find(del_key);
                    if (ct_iter != local_ctm_ptr->end()) {
                        local_ctm_ptr->erase(ct_iter);
                    }
                }
                /*
                 * delete the connction from connection pool
                 */
                int32_t bucket_num = GenerateBucketNum(del_key);
                Mutex& bucket_mutex = connection_bucket_mutex_ptr_->at(bucket_num);
                {
                    MutexLockGuard guard(bucket_mutex);
                    CT_MAP_PTR& ct_map_ptr = connection_pool_buckets_[bucket_num];
                    if (NULL != ct_map_ptr) {
                        CT_MAP::iterator ct_iter = ct_map_ptr->find(del_key);
                        if (ct_iter != ct_map_ptr->end()) {
                            ct_map_ptr->erase(ct_iter);
                        }
                    }
                }
            }
        }

        if (NULL == local_ctm_ptr) {
            continue;
        }
        /*
         * Hash(by modulo for now) all connection into connection_pool buckets
         */
        for (CT_HASH_MAP::iterator iter = local_ctm_ptr->begin();
             iter != local_ctm_ptr->end();
             ++iter) {
            CT_PTR& ct_ptr = iter->second;
            int32_t bucket_num = GenerateBucketNum(iter->first);
            Mutex& bucket_mutex = connection_bucket_mutex_ptr_->at(bucket_num);
            {
                MutexLockGuard guard(bucket_mutex);
                CT_MAP_PTR& ct_map_ptr = connection_pool_buckets_[bucket_num];
                if (NULL == ct_map_ptr) {
                    ct_map_ptr.reset(new CT_MAP());
                }
                ct_map_ptr->insert(std::make_pair(std::move(iter->first), ct_ptr));
            }
        }
    }
    return true;
}




}  // end of namespace libevrpc




/* vim: set expandtab ts=4 sw=4 sts=4 tw=100: */

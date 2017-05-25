﻿/**
 * Copyright (c) 2017 China Mobile IOT.
 * All rights reserved.
**/

#include "internal.h"

int nbiot_device_create( nbiot_device_t         **dev,
                         const char              *endpoint_name,
                         int                      life_time,
                         uint16_t                 local_port,
                         nbiot_write_callback_t   write_func,
                         nbiot_execute_callback_t execute_func )
{
    int ret;
    nbiot_device_t *tmp;

    tmp = (nbiot_device_t*)nbiot_malloc( sizeof(nbiot_device_t) );
    if ( !tmp )
    {
        return NBIOT_ERR_NO_MEMORY;
    }
    else
    {
        nbiot_memzero( tmp, sizeof(nbiot_device_t) );
    }

    ret = nbiot_udp_create( &tmp->socket );
    if ( ret )
    {
        nbiot_free( tmp );

        return ret;
    }

    ret = nbiot_udp_bind( tmp->socket, NULL, local_port );
    if ( ret )
    {
        nbiot_udp_close( tmp->socket );
        nbiot_free( tmp );

        return ret;
    }

    *dev = tmp;
    tmp->next_mid = nbiot_rand();
    tmp->life_time = life_time;
    tmp->write_func = write_func;
    tmp->execute_func = execute_func;
    tmp->endpoint_name = endpoint_name;

    return NBIOT_ERR_OK;
}

#ifdef LOCATION_MALLOC
static void device_free_locations( nbiot_device_t *dev )
{
    nbiot_location_t *next;
    nbiot_location_t *location = dev->locations;

    while ( location )
    {
        next = location->next;
        nbiot_free( location );
        location = next;
    }
    dev->locations = NULL;
}
#endif

static void device_free_nodes( nbiot_device_t *dev )
{
    nbiot_node_t *obj;
    nbiot_node_t *inst;

    obj = dev->nodes;
    while ( obj )
    {
        inst = (nbiot_node_t*)obj->data;
        while ( inst )
        {
            NBIOT_LIST_FREE( inst->data );
            inst = inst->next;
        }

        NBIOT_LIST_FREE( obj->data );
        obj = obj->next;
    }

    NBIOT_LIST_FREE( dev->nodes );
    dev->nodes = NULL;
}

static void device_free_observes( nbiot_device_t *dev )
{
    nbiot_observe_t *obj;
    nbiot_observe_t *inst;

    obj = dev->observes;
    while ( obj )
    {
        inst = (nbiot_observe_t*)obj->list;
        while ( inst )
        {
            NBIOT_LIST_FREE( inst->list );
            inst = inst->next;
        }

        NBIOT_LIST_FREE( obj->list );
        obj = obj->next;
    }

    NBIOT_LIST_FREE( dev->observes );
    dev->observes = NULL;
}

static void device_free_transactions( nbiot_device_t *dev )
{
    nbiot_transaction_t *transaction;

    transaction = dev->transactions;
    while ( transaction )
    {
        nbiot_free( transaction->buffer );
        transaction = transaction->next;
    }

    NBIOT_LIST_FREE( dev->transactions );
    dev->transactions = NULL;
}

void nbiot_device_destroy( nbiot_device_t *dev )
{
    nbiot_sockaddr_destroy( dev->address );
    nbiot_sockaddr_destroy( dev->server );
    nbiot_udp_close( dev->socket );
    device_free_transactions( dev );
#ifdef LOCATION_MALLOC
    device_free_locations( dev );
#endif
    device_free_observes( dev );
    device_free_nodes( dev );
    nbiot_free( dev );
}

static void handle_read( nbiot_device_t    *dev,
                         const nbiot_uri_t *uri,
                         coap_t            *coap )
{
    do
    {
        int ret;
        nbiot_node_t *node;

        node = nbiot_node_find( dev, uri );
        if ( !node )
        {
            coap_set_code( coap, COAP_NOT_FOUND_404 );
            break;
        }

        /* payload */
        coap->buffer[coap->offset++] = 0xff;
        ret = nbiot_node_read( node,
                               uri->flag,
                               coap->buffer + coap->offset,
                               coap->size - coap->offset,
                               false );
        if ( ret > 0 )
        {
            coap->offset += ret;
        }
        else
        {
            coap->offset--;
            coap_set_code( coap, COAP_BAD_REQUEST_400 );
        }
    } while (0);
}

static bool check_writable( nbiot_node_t *node, uint8_t flag )
{
    if ( flag & NBIOT_SET_RESID )
    {
        return ((nbiot_value_t*)node->data)->flag & NBIOT_WRITABLE;
    }

    if ( flag & NBIOT_SET_OBJID )
    {
        if ( flag & NBIOT_SET_INSTID )
        {
            flag |= NBIOT_SET_RESID;
        }
        else
        {
            flag |= NBIOT_SET_INSTID;
        }

        for ( node = (nbiot_node_t*)node->data;
              node != NULL;
              node = node->next )
        {
            if ( !check_writable(node,flag) )
            {
                return false;
            }
        }
    }

    return false;
}

static void handle_write( nbiot_device_t        *dev,
                          const nbiot_uri_t     *uri,
                          coap_t                *coap,
                          const uint8_t         *buffer,
                          size_t                 buffer_len,
                          nbiot_write_callback_t write_func )
{
    do
    {
        int ret;
        nbiot_node_t *node;

        node = nbiot_node_find( dev, uri );
        if ( !node )
        {
            coap_set_code( coap, COAP_NOT_FOUND_404 );
            break;
        }

        /* write */
        ret = nbiot_node_write( node,
                                uri,
                                buffer,
                                buffer_len,
                                write_func );
        coap_set_code( coap, ret );
    } while (0);
}

static void handle_execute( nbiot_device_t          *dev,
                            const nbiot_uri_t       *uri,
                            coap_t                  *coap,
                            const uint8_t           *buffer,
                            size_t                   buffer_len,
                            nbiot_execute_callback_t execute_func )
{
    do
    {
        nbiot_node_t *node;
        nbiot_value_t *data;

        if ( !(uri->flag&NBIOT_SET_RESID) )
        {
            coap_set_code( coap, COAP_BAD_REQUEST_400 );
            break;
        }

        node = nbiot_node_find( dev, uri );
        if ( !node )
        {
            coap_set_code( coap, COAP_NOT_FOUND_404 );
            break;
        }

        data = (nbiot_value_t*)node->data;
        if ( !(data->flag&NBIOT_EXECUTABLE) )
        {
            coap_set_code( coap, COAP_METHOD_NOT_ALLOWED_405 );
            break;
        }

        if ( execute_func )
        {
            execute_func( uri->objid,
                          uri->instid,
                          uri->resid,
                          (nbiot_value_t*)node->data,
                          buffer,
                          buffer_len );
        }

        coap_set_code( coap, COAP_CHANGED_204 );
    } while (0);
}

static void handle_discover( nbiot_device_t          *dev,
                             const nbiot_uri_t       *uri,
                             coap_t                  *coap,
                             const uint8_t           *buffer,
                             size_t                   buffer_len )
{
    
}

static void nbiot_handle_request( nbiot_device_t    *dev,
                                  uint16_t           code,
                                  uint8_t           *buffer,
                                  size_t             buffer_len,
                                  size_t             max_buffer_len )
{
    int ret;
    uint16_t offset;
    uint8_t token[8];
    nbiot_uri_t uri[1];
    coap_t coap[1];
    char *payload = NULL;
    uint16_t payload_len = 0;
    char *uri_query = NULL;
    uint16_t uri_query_len = 0;
    uint32_t accept = UINT32_MAX;
    uint32_t observe = UINT32_MAX;
    uint16_t mid = coap_mid( buffer );
    uint8_t type = coap_type( buffer );
    uint8_t token_len = nbiot_token( buffer, token );

    /* read option */
    nbiot_payload( buffer,
                   buffer_len,
                   &payload,
                   &payload_len );
    nbiot_uri_query( buffer,
                     buffer_len,
                     &uri_query,
                     &uri_query_len,
                     true );
    coap_accept( buffer, buffer_len, &accept );
    coap_observe( buffer, buffer_len, &observe );

    /* initialize response */
    coap->buffer = buffer;
    coap->size = max_buffer_len;
    if ( COAP_TYPE_CON == type )
    {
        coap_init_header( coap,
                          COAP_TYPE_ACK,
                          COAP_CONTENT_205,
                          mid,
                          token,
                          token_len );
    }
    else
    {
        coap_init_header( coap,
                          COAP_TYPE_NON,
                          COAP_CONTENT_205,
                          dev->next_mid++,
                          token,
                          token_len );
    }

    offset = coap->offset;
    do
    {
        ret = nbiot_uri_decode( uri, buffer, buffer_len );
        if ( ret )
        {
            coap_set_code( coap, COAP_BAD_REQUEST_400 );
            break;
        }

        if ( COAP_GET == code )
        {
            if ( observe == 0 )
            {
                nbiot_node_t *node;
                nbiot_observe_t *tmp;

                node = nbiot_node_find( dev, uri );
                if ( !node )
                {
                    coap_set_code( coap, COAP_NOT_FOUND_404 );
                    break;
                }

                tmp = nbiot_observe_add( dev,
                                         uri,
                                         token,
                                         token_len );
                if ( !tmp )
                {
                    coap_set_code( coap, COAP_BAD_REQUEST_400 );
                    break;
                }

                coap_add_observe( coap, tmp->counter++ );
                coap_add_content_type( coap, LWM2M_CONTENT_TLV );
                handle_read( dev, uri, coap );
                break;
            }

            if ( observe == 1 )
            {
                ret = nbiot_observe_del( dev, uri );
                if ( ret )
                {
                    coap_set_code( coap, COAP_NOT_FOUND_404 );
                }
                break;
            }

            if ( accept == LWM2M_CONTENT_LINK )
            {
                /* discover */
                coap_set_code( coap, COAP_METHOD_NOT_ALLOWED_405 );
                break;
            }

            /* read */
            coap_add_content_type( coap, LWM2M_CONTENT_TLV );
            handle_read( dev, uri, coap );
            break;
        }

        if ( COAP_POST == code )
        {
            if ( uri->flag & NBIOT_SET_RESID )
            {
                /* execute */
                handle_execute( dev,
                                uri,
                                coap,
                                payload,
                                payload_len,
                                dev->execute_func );
                break;
            }

            if ( uri->flag & NBIOT_SET_INSTID )
            {
                /* write - partial update */
                handle_write( dev,
                              uri,
                              coap,
                              payload,
                              payload_len,
                              dev->write_func );
                break;
            }

            /* create */
            coap_set_code( coap, COAP_METHOD_NOT_ALLOWED_405 );
            break;
        }

        if ( COAP_PUT == code )
        {
            if ( uri_query )
            {
                /* attributes */
                coap_set_code( coap, COAP_METHOD_NOT_ALLOWED_405 );
                break;
            }

            /* write - replace */
            coap_set_code( coap, COAP_METHOD_NOT_ALLOWED_405 );
            break;
        }

        if ( COAP_DELETE == code )
        {
            /* delete */
            coap_set_code( coap, COAP_METHOD_NOT_ALLOWED_405 );
            break;
        }

        /* error */
        coap_set_code( coap, COAP_BAD_REQUEST_400 );
    } while (0);

    code = coap_code( coap->buffer );
    if ( code != COAP_CHANGED_204 &&
         code != COAP_CONTENT_205 )
    {
        coap->offset = offset;
    }

    nbiot_free( payload );
    nbiot_free( uri_query );
    nbiot_send_buffer( dev->socket,
                       dev->server,
                       coap->buffer,
                       coap->offset );
}

static void nbiot_handle_transaction( nbiot_device_t *dev,
                                      uint16_t        code,
                                      uint8_t        *buffer,
                                      size_t          buffer_len,
                                      size_t          max_buffer_len )
{
    uint16_t mid = coap_mid( buffer );
    uint8_t type = coap_type( buffer );
    nbiot_transaction_t *transaction = NULL;

    if ( COAP_TYPE_ACK == type || COAP_TYPE_RST == type )
    {
        transaction = (nbiot_transaction_t*)NBIOT_LIST_GET( dev->transactions, mid );
    }
    else
    {
        uint16_t token_len;
        const uint8_t *token;

        token_len = coap_token( buffer, &token );
        if ( token_len )
        {
            for ( transaction = dev->transactions;
                  transaction != NULL;
                  transaction = transaction->next )
            {
                const uint8_t *token_src;
                if ( coap_token(transaction->buffer,&token_src) == token_len )
                {
                    if ( nbiot_memcmp(token,token_src,token_len) == 0 )
                    {
                        /* found */
                        break;
                    }
                }
            }
        }
    }

    if ( transaction )
    {
        if ( COAP_TYPE_RST == type ||
             COAP_UNAUTHORIZED_401 != code ||
             COAP_MAX_RETRANSMIT <= transaction->counter )
        {
            nbiot_transaction_del( dev,
                                   mid,
                                   buffer,
                                   buffer_len );
        }
        else
        {
            transaction->ack = false;
            transaction->time += COAP_RESPONSE_TIMEOUT;
        }
    }

    if ( COAP_TYPE_CON == type )
    {
        int ret;
        coap_t coap[1];

        coap->buffer = buffer;
        coap->size = max_buffer_len;
        ret = coap_init_header( coap,
                                COAP_TYPE_ACK,
                                0,
                                mid,
                                NULL,
                                0 );
        if ( ret )
        {
            nbiot_send_buffer( dev->socket,
                               dev->server,
                               coap->buffer,
                               coap->offset );
        }
    }

    if ( COAP_TYPE_RST == type )
    {
        /* reset */
        dev->state = STATE_SVR_RESET;
    }
}

static void nbiot_handle_buffer( nbiot_device_t *dev,
                                 uint8_t        *buffer,
                                 size_t          buffer_len,
                                 size_t          max_buffer_len )
{
    uint16_t code = coap_code( buffer );
    if ( COAP_GET <= code && COAP_DELETE >= code )
    {
        if ( dev->state == STATE_REGISTERED ||
             dev->state == STATE_REG_UPDATE_NEEDED ||
             dev->state == STATE_REG_UPDATE_PENDING )
        {
            nbiot_handle_request( dev,
                                  code,
                                  buffer,
                                  buffer_len,
                                  max_buffer_len );
        }
    }
    else
    {
        nbiot_handle_transaction( dev,
                                  code,
                                  buffer,
                                  buffer_len,
                                  max_buffer_len );
    }
}

int nbiot_device_connect( nbiot_device_t *dev,
                          const char     *server_uri,
                          int             timeout )
{
    int ret;
    time_t last;
    time_t curr;
    const char *addr;
    const char *port;
    uint8_t buffer[NBIOT_SOCK_BUF_SIZE];

    if ( !nbiot_strncmp(server_uri,"coaps://",8) )
    {
        addr = server_uri + 8;
    }
    else if ( !nbiot_strncmp(server_uri,"coap://",7) )
    {
        addr = server_uri + 7;
    }
    else
    {
        return NBIOT_ERR_INVALID_URI;
    }

    port = nbiot_strrchr( server_uri, -1, ':' );
    if ( !port )
    {
        return NBIOT_ERR_INVALID_URI;
    }

    addr = nbiot_strdup( addr, port - addr );
    if ( !addr )
    {
        return NBIOT_ERR_NO_MEMORY;
    }

    ret = nbiot_udp_connect( dev->socket,
                             addr,
                             nbiot_atoi(++port,-1),
                             &dev->server );
    nbiot_free( (void*)addr );
    if ( ret )
    {
        return ret;
    }

    /* registraction */
    ret = nbiot_register_start( dev, buffer, sizeof(buffer) );
    if ( ret )
    {
        return NBIOT_ERR_INTERNAL;
    }

    last = nbiot_time();
    do
    {
        int buffer_len = nbiot_recv_buffer( dev->socket,
                                           &dev->address,
                                            buffer,
                                            sizeof(buffer) );
        if ( buffer_len > 0 )
        {
            if ( nbiot_sockaddr_equal(dev->address,dev->server) )
            {
                nbiot_handle_buffer( dev,
                                     buffer,
                                     buffer_len,
                                     sizeof(buffer) );
            }
        }

        curr = nbiot_time();
        nbiot_transaction_step( dev,
                                curr,
                                buffer,
                                sizeof(buffer) );

        /* ok */
        if ( STATE_REGISTERED == dev->state )
        {
            return NBIOT_ERR_OK;
        }

        /* failed */
        if ( STATE_REG_FAILED == dev->state )
        {
            return NBIOT_ERR_REG_FAILED;
        }

        /* continue */
        nbiot_sleep( 100 );
    } while( curr <= last + timeout );

    return NBIOT_ERR_TIMEOUT;
}

void nbiot_device_close( nbiot_device_t *dev,
                         int             timeout )
{
    int ret;
    uint8_t buffer[NBIOT_SOCK_BUF_SIZE];

    ret = nbiot_deregister( dev, buffer, sizeof(buffer) );
    if ( ret == COAP_NO_ERROR )
    {
        time_t last;
        time_t curr;

        last = nbiot_time();
        do
        {
            int buffer_len = nbiot_recv_buffer( dev->socket,
                                                &dev->address,
                                                buffer,
                                                sizeof( buffer ) );
            if ( buffer_len > 0 )
            {
                if ( nbiot_sockaddr_equal( dev->address, dev->server ) )
                {
                    nbiot_handle_buffer( dev,
                                         buffer,
                                         buffer_len,
                                         sizeof( buffer ) );
                }
            }

            curr = nbiot_time();
            nbiot_transaction_step( dev,
                                    curr,
                                    buffer,
                                    sizeof( buffer ) );

            /* ok */
            if ( dev->state == STATE_REG_FAILED ||
                 dev->state == STATE_DEREGISTERED )
            {
                break;
            }

            /* continue */
            nbiot_sleep( 100 );
        } while ( curr <= last + timeout );
    }
}

int nbiot_device_step( nbiot_device_t *dev,
                       int             timeout )
{
    time_t last;
    time_t curr;
    uint8_t buffer[NBIOT_SOCK_BUF_SIZE];

    last = nbiot_time();
    do
    {
        int buffer_len = nbiot_recv_buffer( dev->socket,
                                           &dev->address,
                                            buffer,
                                            sizeof(buffer) );
        if ( buffer_len > 0 )
        {
            if ( nbiot_sockaddr_equal(dev->address,dev->server) )
            {
                nbiot_handle_buffer( dev,
                                     buffer,
                                     buffer_len,
                                     sizeof(buffer) );
            }
        }

        curr = nbiot_time();
        nbiot_register_step( dev,
                             curr,
                             buffer,
                             sizeof(buffer) );
        nbiot_transaction_step( dev,
                                curr,
                                buffer,
                                sizeof(buffer) );
        nbiot_observe_step( dev,
                            curr,
                            buffer,
                            sizeof(buffer) );
        nbiot_sleep( 100 );
    } while ( curr <= last + timeout );

    return STATE_ERROR( dev );
}

int nbiot_resource_add( nbiot_device_t *dev,
                        uint16_t        objid,
                        uint16_t        instid,
                        uint16_t        resid,
                        nbiot_value_t  *data )
{
    nbiot_node_t *obj;
    nbiot_node_t *res;
    nbiot_node_t *inst;
    bool obj_new = false;
    bool inst_new = false;

    obj = (nbiot_node_t*)NBIOT_LIST_GET( dev->nodes, objid );
    if ( !obj )
    {
        obj = (nbiot_node_t*)nbiot_malloc( sizeof(nbiot_node_t) );
        if ( !obj )
        {
            return NBIOT_ERR_NO_MEMORY;
        }

        obj_new = true;
        nbiot_memzero( obj, sizeof(nbiot_node_t) );
        obj->id = objid;
        dev->nodes = (nbiot_node_t*)NBIOT_LIST_ADD( dev->nodes, obj );
    }

    inst = (nbiot_node_t*)NBIOT_LIST_GET( obj->data, instid );
    if ( !inst )
    {
        inst = (nbiot_node_t*)nbiot_malloc( sizeof(nbiot_node_t) );
        if ( !inst )
        {
            if ( obj_new )
            {
                dev->nodes = (nbiot_node_t*)NBIOT_LIST_DEL( dev->nodes, objid, NULL );
                nbiot_free( obj );
            }

            return NBIOT_ERR_NO_MEMORY;
        }

        inst_new = true;
        nbiot_memzero( inst, sizeof(nbiot_node_t) );
        inst->id = instid;
        obj->data = NBIOT_LIST_ADD( obj->data, inst );
    }

    res = (nbiot_node_t*)NBIOT_LIST_GET( inst->data, instid );
    if ( !res )
    {
        res = (nbiot_node_t*)nbiot_malloc( sizeof(nbiot_node_t) );
        if ( !res )
        {
            if ( inst_new )
            {
                obj->data = NBIOT_LIST_DEL( obj->data, instid, NULL );
                nbiot_free( inst );
            }

            if ( obj_new )
            {
                dev->nodes = (nbiot_node_t*)NBIOT_LIST_DEL( dev->nodes, objid, NULL );
                nbiot_free( obj );
            }

            return NBIOT_ERR_NO_MEMORY;
        }

        nbiot_memzero( res, sizeof(nbiot_node_t) );
        res->id = resid;
        inst->data = NBIOT_LIST_ADD( inst->data, res );
    }

    /* not free */
    res->data = data;
    if ( !STATE_ERROR(dev) )
    {
        dev->state = STATE_REG_UPDATE_NEEDED;
    }

    return NBIOT_ERR_OK;
}

int nbiot_resource_del( nbiot_device_t *dev,
                        uint16_t        objid,
                        uint16_t        instid,
                        uint16_t        resid )
{
    nbiot_node_t *obj;
    nbiot_node_t *res;
    nbiot_node_t *inst;

    obj = (nbiot_node_t*)NBIOT_LIST_GET( dev->nodes, objid );
    if ( obj )
    {
        inst = (nbiot_node_t*)NBIOT_LIST_GET( obj->data, instid );
        if ( inst )
        {
            inst->data = NBIOT_LIST_DEL( inst->data, resid, &res );
            if ( res )
            {
                nbiot_free( res );

                if ( inst->data )
                {
                    obj->data = NBIOT_LIST_DEL( obj->data, instid, NULL );
                    nbiot_free( inst );
                }

                if ( obj->data )
                {
                    dev->nodes = (nbiot_node_t*)NBIOT_LIST_DEL( dev->nodes, objid, NULL );
                    nbiot_free( obj );
                }

                if ( !STATE_ERROR(dev) )
                {
                    dev->state = STATE_REG_UPDATE_NEEDED;
                }

                return NBIOT_ERR_OK;
            }
        }
    }

    return NBIOT_ERR_NOT_FOUND;
}

void nbiot_device_sync( nbiot_device_t *dev )
{
    if ( STATE_ERROR( dev ) )
    {
        time_t curr = nbiot_time();
        uint8_t buffer[NBIOT_SOCK_BUF_SIZE];

        nbiot_register_step( dev,
                             curr,
                             buffer,
                             sizeof(buffer) );
        nbiot_observe_step( dev,
                            curr,
                            buffer,
                            sizeof(buffer) );
    }
}
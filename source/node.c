﻿/**
 * Copyright (c) 2017 China Mobile IOT.
 * All rights reserved.
**/

#include "internal.h"

static int nbiot_node_length( nbiot_node_t *node,
                              uint8_t       flag,
                              bool          updated )
{
    if ( flag & NBIOT_SET_RESID )
    {
        nbiot_value_t *data = (nbiot_value_t*)node->data;
        if ( !updated || (data->flag&NBIOT_UPDATED) )
        {
            if ( data->type == NBIOT_BOOLEAN )
            {
                return 1;
            }

            if ( data->type == NBIOT_INTEGER )
            {
                uint8_t len = 0;
                int64_t val = data->value.as_int;

                while ( val )
                {
                    ++len;
                    val >>= 8;
                }

                return len;
            }

            if ( data->type == NBIOT_FLOAT )
            {
                if ( data->value.as_float < -(double)FLT_MAX ||
                     data->value.as_float >  (double)FLT_MAX )
                {
                    return 8;
                }
                else
                {
                    return 4;
                }
            }

            if ( data->type == NBIOT_STRING ||
                 data->type == NBIOT_BINARY )
            {
                return data->value.as_buf.len;
            }
        }
    }
    else if ( flag & NBIOT_SET_OBJID )
    {
        int len = 0;

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
            int ret = nbiot_node_length( node,
                                         flag,
                                         updated );
            if ( ret )
            {
                len += nbiot_tlv_length( node->id, ret );
            }
        }

        return len;
    }

    return 0;
}

int nbiot_node_read( nbiot_node_t *node,
                     uint8_t       flag,
                     uint8_t      *buffer,
                     size_t        buffer_len,
                     bool          updated )
{
    if ( flag & NBIOT_SET_RESID )
    {
        nbiot_value_t *data = (nbiot_value_t*)node->data;
        if ( !updated || (data->flag&NBIOT_UPDATED) )
        {
            uint8_t array[8];
            uint8_t *value = NULL;
            size_t value_len = 0;

            data->flag &= ~NBIOT_UPDATED;
            if ( data->type == NBIOT_BOOLEAN )
            {
                array[0] = data->value.as_bool ? 1 : 0;
                value_len = 1;
                value = array;
            }

            if ( data->type == NBIOT_INTEGER )
            {
                int64_t tmp = data->value.as_int;
                uint8_t idx = sizeof(array);

                while ( tmp )
                {
                    array[--idx] = (uint8_t)(tmp & 0xff);
                    tmp >>= 8;
                }

                value = array + idx;
                value_len = sizeof(array) - idx;
            }

            if ( data->type == NBIOT_FLOAT )
            {
                int64_t tmp;
                uint8_t idx;

                if ( data->value.as_float < -(double)FLT_MAX ||
                     data->value.as_float >  (double)FLT_MAX )
                {
                    tmp = *(int64_t*)(&data->value.as_float);
                    value_len = 8;
                }
                else
                {
                    float f = (float)data->value.as_float;
                    tmp = *(uint32_t*)(&f);
                    value_len = 4;
                }

                idx = value_len;
                while ( idx )
                {
                    array[--idx] = (uint8_t)(tmp & 0xff);
                    tmp >>= 8;
                }
                value = array;
            }

            if ( data->type == NBIOT_STRING ||
                 data->type == NBIOT_BINARY )
            {
                value = (uint8_t*)data->value.as_buf.val;
                value_len = data->value.as_buf.len;
            }

            if ( value &&
                 value_len <= buffer_len )
            {
                nbiot_memmove( buffer,
                               value,
                               value_len );

                return value_len;
            }
        }
    }
    else if ( flag & NBIOT_SET_OBJID )
    {
        int ret;
        int len;
        int num = 0;
        uint8_t type;

        if ( flag & NBIOT_SET_INSTID )
        {
            flag |= NBIOT_SET_RESID;
            type = TLV_TYPE_RESOURCE;
        }
        else
        {
            flag |= NBIOT_SET_INSTID;
            type = TLV_TYPE_OBJECT_INSTANCE;
        }

        for ( node = (nbiot_node_t*)node->data;
              node != NULL;
              node = node->next )
        {
            len = nbiot_node_length( node, flag, updated );
            if ( len )
            {
                ret = nbiot_tlv_encode( buffer + num,
                                        buffer_len - num,
                                        type,
                                        node->id,
                                        NULL,
                                        len );
                if ( ret )
                {
                    ret += nbiot_node_read( node,
                                            flag,
                                            buffer + num + ret,
                                            buffer_len - num - ret,
                                            updated );
                }
            }

            len = nbiot_tlv_length( node->id, len );
            if ( len == ret )
            {
                num += len;
            }
        }

        return num;
    }

    return 0;
}

int nbiot_node_write( nbiot_node_t          *node,
                      const nbiot_uri_t     *uri,
                      const uint8_t         *buffer,
                      size_t                 buffer_len,
                      nbiot_write_callback_t write_func )
{
    if ( uri->flag & NBIOT_SET_RESID )
    {
        nbiot_value_t *data = (nbiot_value_t*)node->data;
        if ( !(data->flag&NBIOT_WRITABLE) )
        {
            return COAP_METHOD_NOT_ALLOWED_405;
        }

        if ( data->type == NBIOT_BOOLEAN )
        {
            data->value.as_bool = buffer[0];
        }

        if ( data->type == NBIOT_FLOAT ||
             data->type == NBIOT_INTEGER )
        {
            int value_len = buffer_len;

            data->value.as_int = 0;
            while ( buffer_len )
            {
                buffer_len--;
                data->value.as_int <<= 8;
                data->value.as_int += *buffer++;
            }

            if ( value_len == 4 &&
                 data->type == NBIOT_FLOAT )
            {
                uint32_t u = (uint32_t)data->value.as_int;
                data->value.as_float = *(float*)&u;
            }
        }

        if ( data->type == NBIOT_STRING ||
             data->type == NBIOT_BINARY )
        {
            nbiot_free( data->value.as_buf.val );
            data->value.as_buf.val = nbiot_strdup( (char*)buffer, buffer_len );
            data->value.as_buf.len = buffer_len;
        }

        if ( write_func )
        {
            write_func( uri->objid,
                        uri->instid,
                        uri->resid,
                        data );
        }

        return COAP_CHANGED_204;
    }
    else if ( uri->flag & NBIOT_SET_OBJID )
    {
        uint16_t *id;
        nbiot_uri_t tmp[1];

        *tmp = *uri;
        if ( tmp->flag & NBIOT_SET_INSTID )
        {
            id = &tmp->resid;
            tmp->flag |= NBIOT_SET_RESID;
        }
        else
        {
            id = &tmp->instid;
            tmp->flag |= NBIOT_SET_INSTID;
        }

        while ( buffer_len )
        {
            int ret;
            size_t value_len;
            nbiot_node_t *child;
            const uint8_t *value;

            ret = nbiot_tlv_decode( buffer,
                                    buffer_len,
                                    NULL,
                                    id,
                                    &value,
                                    &value_len );
            if ( ret )
            {
                child = (nbiot_node_t*)NBIOT_LIST_GET( node->data, *id );
                if ( !child )
                {
                    return COAP_NOT_FOUND_404;
                }

                buffer += ret;
                buffer_len -= ret;
                ret = nbiot_node_write( child,
                                        tmp,
                                        value,
                                        value_len,
                                        write_func );
                if ( COAP_CHANGED_204 != ret )
                {
                    return ret;
                }
            }
            else
            {
                return COAP_BAD_REQUEST_400;
            }
        }

        return COAP_CHANGED_204;
    }

    return COAP_BAD_REQUEST_400;
}

nbiot_node_t* nbiot_node_find( nbiot_device_t    *dev,
                               const nbiot_uri_t *uri )
{
    nbiot_node_t *node = NULL;

    if ( uri->flag & NBIOT_SET_OBJID )
    {
        node = (nbiot_node_t*)NBIOT_LIST_GET( dev->nodes, uri->objid );
        if ( node && (uri->flag&NBIOT_SET_INSTID) )
        {
            node = (nbiot_node_t*)NBIOT_LIST_GET( node->data, uri->instid );
            if ( node && (uri->flag&NBIOT_SET_RESID) )
            {
                node = (nbiot_node_t*)NBIOT_LIST_GET( node->data, uri->resid );
            }
        }
    }

    return node;
}
#include<stdio.h>
#include<stdlib.h>
#include<pthread.h>
#include<string.h>
#include<unistd.h>

typedef char ALIGN[16];

pthread_mutex_t global_malloc_lock;

union header
{
    struct {
        size_t size;
        unsigned is_free;
        union header * next;
    } s;
    ALIGN stub;
};

typedef union header header_t;

header_t *head = NULL, *tail = NULL;

header_t * get_free_block(size_t size){
    header_t *curr = head;
    while (curr)
    {
        if (curr->s.is_free && curr->s.size >= size)
        {
            return curr;
        }
        curr = curr->s.next;

    }

    return NULL;
}

void * malloc(size_t size){
    printf("My Malloc");
    if (size == 0)
    {
        return NULL;
    }
    size_t total_size = size + sizeof(header_t);
    void * block;
    header_t *header;

    pthread_mutex_lock(&global_malloc_lock);

    header = get_free_block(size);

    if (header)
    {
        header->s.is_free = 0;
        pthread_mutex_unlock(&global_malloc_lock);
        return (void *)(header + 1);
    }

    block = sbrk(total_size);
    if (block == (void *) -1)
    {
        pthread_mutex_unlock(&global_malloc_lock);
        return NULL;
    }
    header = block;
    header->s.size = size;
    header->s.is_free = 0;
    header->s.next = NULL;

    if (!head)
    {
        head = header;
    }

    if(tail){
        tail->s.next = header;
    }

    tail = header;
    pthread_mutex_unlock(&global_malloc_lock);

    return block;
}

void free(void *block) {
    printf("My free");
    header_t *header, *tmp;
    void *program_break;
    if (!block)
    {
        return;
    }

    pthread_mutex_lock(&global_malloc_lock);
    header = (header_t *)block - 1; // 指向 header 起始地址

    program_break = sbrk(0);

    if ((char *)block + header->s.size == program_break)
    {
        // 正好是最后一个block
        if (head == tail)
        {
            // 最后一块
            head = tail = NULL;
        }else
        {
            tmp = head;
            while (tmp)
            {
                if (tmp->s.next == tail)
                {
                    tmp->s.next = NULL;
                    tail = tmp;
                }
                tmp = tmp->s.next;
            }
        }
        sbrk(0 - header->s.size - sizeof(header_t)); // 释放 header + 实际内存区域
        pthread_mutex_unlock((&global_malloc_lock));
        return;
    }
    header->s.is_free = 1; // 只标记当前块为可用，实际并未释放内存
    pthread_mutex_unlock((&global_malloc_lock));
}

void *calloc(size_t num, size_t nsize) {
    void * block;
    size_t size;
    if (!num || !nsize)
    {
        return NULL;
    }
    size = num * nsize;

    /* check mul overflow */
    if (nsize != size / num)
    {
        return NULL;
    }
    block = malloc(size);
    if(!block) return NULL;
    memset(block, 0, size);
    return block;
}

void *realloc(void *block, size_t size){
    printf("My realloc");
    header_t *header;
    void *ret;
    if (!block || !size)
    {
        return NULL;
    }
    header = (header_t *)block - 1;

    if (header->s.size >= size) // 无需另外申请
    {
        return block;
    }

    ret = malloc(size);

    if (ret)
    {
        memcpy(ret, block, header->s.size);
        free(block);
    }
    return ret;

}

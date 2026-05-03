#ifndef LINKED_LIST_HPP
#define LINKED_LIST_HPP

#include <iostream>

template <typename T>
class LinkedList {
private:
    struct Node {
        T data;
        Node* next;
        Node(T val) : data(val), next(nullptr) {}
    };

    Node* head;

public:
    LinkedList() : head(nullptr) {}

    // Destructor to clean up memory
    ~LinkedList() {
        Node* current = head;
        while (current != nullptr) {
            Node* next = current->next;
            delete current;
            current = next;
        }
    }

    // Add element to the front
    void push_front(T val) {
        Node* newNode = new Node(val);
        newNode->next = head;
        head = newNode;
    }

    // Print the list
    void display() const {
        Node* temp = head;
        while (temp != nullptr) {
            std::cout << temp->data << " -> ";
            temp = temp->next;
        }
        std::cout << "nullptr" << std::endl;
    }
};

#endif

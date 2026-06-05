# AomNetworkHook

DLL responsável pela correção de rede usada por **Age of Mythology: The Titans 1.03** quando o jogo escolhe o adaptador errado para partidas multiplayer via Hamachi, Bottles/Wine ou ambientes com múltiplas interfaces.

Repositório: [DerexScript/AomNetworkHook](https://github.com/DerexScript/AomNetworkHook)

Release 1.0: [AomNetWork](https://github.com/DerexScript/AomNetworkHook/releases/tag/1.0)

## O Que Este Projeto Faz

O `AomNetworkHook.dll` é carregado dentro do processo do `aomx.exe`.

Ele lê o arquivo `AomNetworkPatch.ini`, identifica o adaptador configurado em `Adapter` e instala um hook em uma chamada específica do WinSock:

```text
WSAIoctl
```

O hook atua apenas quando o jogo chama:

```text
SIO_ROUTING_INTERFACE_QUERY
```

Essa consulta é o ponto decisivo. Ela pergunta ao Windows qual interface local deve ser usada para alcançar um determinado destino de rede.

Sem correção, o Windows pode responder com o IP da Ethernet local, por exemplo:

```text
10.0.0.198
```

Com o hook, essa resposta é substituída pelo IP do adaptador configurado, por exemplo:

```text
25.44.222.172
```

Assim o jogo passa a enxergar o IP correto no lobby e a abrir os sockets de multiplayer usando o adaptador esperado.

## Relação Com AomNetworkPatch

Esta DLL foi feita para ser carregada pelo launcher `AomNetworkPatch.exe`.

O launcher está neste repositório:

[DerexScript/AomNetworkPatch](https://github.com/DerexScript/AomNetworkPatch)

As responsabilidades são separadas:

- `AomNetworkPatch`: inicia o jogo, injeta a DLL e repassa argumentos.
- `AomNetworkHook`: roda dentro do jogo e corrige a consulta de interface/rota.

A DLL sozinha não inicia o jogo. Ela precisa ser carregada no processo do `aomx.exe`. O método recomendado é usar o `AomNetworkPatch.exe`.

## Arquivos Necessários

No uso normal, estes três arquivos ficam juntos:

```text
AomNetworkPatch.exe
AomNetworkHook.dll
AomNetworkPatch.ini
```

O release `1.0` deste repositório publica o arquivo `AomNetwork.zip`, contendo os três arquivos:

[Baixar release 1.0](https://github.com/DerexScript/AomNetworkHook/releases/tag/1.0)

O mesmo pacote também está disponível no release do [`AomNetworkPatch`](https://github.com/DerexScript/AomNetworkPatch), porque os dois projetos trabalham juntos.

## Configuração

A DLL lê o mesmo `AomNetworkPatch.ini` que fica ao lado do launcher e da própria DLL.

Exemplo:

```ini
[AomNetworkPatch]
GamePath=C:\Games\Age of Mythology\aomx.exe
Adapter=Hamachi
```

Para o `AomNetworkHook`, a chave importante é:

```ini
Adapter=Hamachi
```

`GamePath` é usado pelo launcher. A DLL não precisa iniciar o jogo; ela só precisa saber qual adaptador deve forçar.

## Como O Adaptador É Encontrado

O valor de `Adapter` não precisa ser exatamente igual ao nome completo do adaptador.

Exemplos:

```ini
Adapter=Hamachi
```

ou:

```ini
Adapter=LogMeIn Hamachi Virtual Ethernet Adapter
```

A DLL faz uma busca parcial e sem diferenciar maiúsculas/minúsculas nos campos do adaptador retornados pelo Windows:

- `FriendlyName`
- `Description`
- `AdapterName`

Isso permite que uma configuração curta como `Hamachi` encontre o adaptador real `LogMeIn Hamachi Virtual Ethernet Adapter`.

Depois de encontrar o adaptador, a DLL pega o primeiro IPv4 utilizável dele e guarda esse endereço para responder à consulta do jogo.

## O Problema Que O Hook Resolve

O jogo usa APIs antigas de rede e comportamento associado a DirectPlay/WinSock. Durante a preparação do multiplayer, ele pergunta ao sistema qual interface local deve ser usada para determinada rota.

Em uma máquina com Ethernet e Hamachi, a resposta padrão do sistema pode apontar para a Ethernet:

```text
10.0.0.198
```

O jogo então passa a usar esse IP como referência local. Isso aparece de duas formas:

- o lobby mostra o IP local errado;
- a conexão com outros jogadores via Hamachi fica presa tentando conectar.

O ponto que resolveu foi interceptar `SIO_ROUTING_INTERFACE_QUERY`.

Essa chamada acontece antes de o jogo abrir alguns sockets importantes. Se a resposta for corrigida nesse momento, o próprio jogo passa a pedir `bind` no IP correto, sem depender de corrigir o socket depois.

## O Que É Hookado

A DLL hooka a função:

```cpp
WSAIoctl
```

Essa função é genérica: várias operações de controle de socket passam por ela. Por isso o hook não altera todas as chamadas.

Ele verifica o código da operação:

```cpp
SIO_ROUTING_INTERFACE_QUERY
```

Quando o código é outro, a chamada segue normalmente.

Quando o código é `SIO_ROUTING_INTERFACE_QUERY`, o fluxo fica assim:

1. O jogo chama `WSAIoctl`.
2. O Windows responde qual interface local usaria.
3. A DLL substitui essa resposta por um `sockaddr_in` contendo o IPv4 do adaptador configurado.
4. A função retorna sucesso para o jogo.
5. O jogo passa a acreditar que a interface correta é o adaptador escolhido.

De forma simples: o jogo pergunta "qual IP local eu devo usar para sair por essa rota?", e o hook responde "use este IP aqui", que é o IP do adaptador configurado no `.ini`.

## Por Que Não Hookar Tudo

Durante a investigação, várias APIs de rede podem ser observadas: `bind`, `sendto`, `recvfrom`, `inet_ntoa`, listas de adaptadores e outras consultas.

Mas a solução final ficou menor porque o ponto necessário foi encontrado:

```text
SIO_ROUTING_INTERFACE_QUERY
```

Interceptar esse ponto é melhor do que corrigir sintomas depois, porque ele muda a decisão original do jogo. Quando essa resposta vem correta, o jogo já abre os sockets importantes usando o IP correto.

Isso reduz o risco de efeitos colaterais e mantém a DLL focada em uma única responsabilidade.

## Por Que Isso É Essencial Em Bottles/Wine

Em Windows nativo, é possivel influenciar qual adaptador o jogo vai escolher, desabilitando todos adaptadores com excessão do adaptador do Hamachi, Abrir o jogo e ir para o modo Multiplayer, e Depois voltar a reativar os adaptadores incluindo o da sua internet, que antes foi desativado para que o tunnel do Hamachi volte a ficar online.

Em Bottles/Wine, essa estratégia é mais frágil. O jogo está rodando dentro de uma camada de compatibilidade, e a forma como ele enxerga as interfaces pode não refletir perfeitamente o que foi configurado no sistema host.

Por isso a abordagem deste projeto é agir dentro do processo do jogo, no momento exato em que ele consulta a interface local. Em vez de tentar convencer o sistema inteiro a priorizar um adaptador, a DLL corrige a resposta que interessa ao jogo.

Para cenários com Wine/Bottles, essa abordagem tende a ser essencial porque a solução manual do Windows não é diretamente replicável.

## Build

O projeto foi criado como **Dynamic Link Library** no CodeBlocks.

Ele usa MinHook para instalar o hook em `WSAIoctl`.

Bibliotecas usadas:

```text
ws2_32
iphlpapi
kernel32
user32
```

O binário final esperado é:

```text
AomNetworkHook.dll
```

Para uso real, coloque a DLL ao lado de:

```text
AomNetworkPatch.exe
AomNetworkPatch.ini
```

## Escopo

Esta DLL não inicia o jogo.

Ela também não altera o executável no disco, não grava patch permanente e não faz varredura de memória do jogo.

O escopo é propositalmente pequeno:

- ler o adaptador configurado;
- resolver o IPv4 desse adaptador;
- hookar `WSAIoctl`;
- corrigir apenas `SIO_ROUTING_INTERFACE_QUERY`.

O resultado esperado é que o jogo use o IP do adaptador configurado no lobby e nas conexões multiplayer.
